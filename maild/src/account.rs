//! Per-account threads, each with its own connection and `Db`: the watcher (IDLE on Inbox, says
//! when something came) and the worker (the job queue: syncs, and later actions, fetches, search).

use crate::config::{Account, Paths};
use crate::db::{Db, Role};
use crate::hub::{Event, Hub, lock};
use crate::imap::{self, AUTH_FAILED, Conn, KillSlot};
use crate::{actions, fetch, notify, search, secret, smtp, sync};
use serde::Serialize;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender, channel};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// IDLE is renewed well before the 29 minutes after which a server may drop it.
pub const IDLE_TIMEOUT: Duration = Duration::from_secs(20 * 60);
/// Waits between failed connection attempts; the last one repeats.
const BACKOFF: [Duration; 3] = if cfg!(test) {
    [Duration::from_millis(10), Duration::from_millis(20), Duration::from_millis(50)]
} else {
    [Duration::from_secs(5), Duration::from_secs(15), Duration::from_secs(60)]
};
/// A session that lasted this long was healthy: when it breaks, reconnect without waiting.
const STEADY: Duration = Duration::from_secs(60);
/// The worker runs SyncAll at least this often.
const PERIOD: Duration = Duration::from_secs(15 * 60);

/// Worker jobs.
pub enum Job {
    SyncFolder(i64),
    SyncAll,
    Actions,
    Fetch { message: i64, reply: Sender<Result<(), String>> },
    FetchRemote { folder_path: String, uid: u32, reply: Sender<Result<i64, String>> },
    Attachment { message: i64, index: u32, reply: Sender<Result<PathBuf, String>> },
    Search { query: String, token: u64 },
    Append { outbox: i64, role: Role, raw: Vec<u8> },
}

/// The watcher's connection state.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Status { Connecting, Online, Offline(String), AuthFailed }

/// What both threads of an account share.
struct Shared {
    email: String,
    hub: Hub,
    connect: Connector,
    cache: PathBuf,
    /// Set once, by `Handle::stop` or a dead worker. Statuses are set under this lock, so none
    /// comes after it.
    stop: Mutex<bool>,
    watch_kill: KillSlot,
    work_kill: KillSlot,
}

impl Shared {
    fn stopped(&self) -> bool {
        *lock(&self.stop)
    }

    /// False once stopped: the account keeps its last status.
    fn status(&self, s: Status) -> bool {
        let stop = lock(&self.stop);
        if !*stop {
            self.hub.set_status(&self.email, s);
        }
        !*stop
    }

    /// `why`: the final status. Both connections break, so neither thread stays blocked.
    fn halt(&self, why: Option<String>) {
        {
            let mut stop = lock(&self.stop);
            if let (false, Some(e)) = (*stop, why) {
                self.hub.set_status(&self.email, Status::Offline(e));
            }
            *stop = true;
        }
        imap::kill(&self.watch_kill);
        imap::kill(&self.work_kill);
    }
}

/// The threads run until `stop`.
#[derive(Clone)]
pub struct Handle {
    pub email: String,
    pub jobs: Sender<Job>,
    retry: Sender<()>,
    shared: Arc<Shared>,
    threads: Arc<Mutex<Vec<JoinHandle<()>>>>,
}

impl Handle {
    /// Breaks both connections (after sleep): blocked reads end at once, the threads reconnect.
    pub fn interrupt(&self) {
        imap::kill(&self.shared.watch_kill);
        imap::kill(&self.shared.work_kill);
    }

    /// "Sync now": queues Actions and SyncAll, wakes a watcher that waits out AuthFailed or a backoff.
    pub fn retry(&self) {
        let _ = self.retry.send(());
        let _ = self.jobs.send(Job::Actions);
        let _ = self.jobs.send(Job::SyncAll);
    }

    /// Both threads end at their next step; an interrupted job is not rerun. A watcher inside
    /// `connect` ends when that returns (up to the connect + I/O timeouts).
    pub fn stop(&self) {
        self.shared.halt(None);
        // Wake both: the watcher from a wait, the worker from its queue (it checks `stop` first).
        let _ = self.retry.send(());
        let _ = self.jobs.send(Job::SyncAll);
    }

    /// `stop`, then wait until both threads have exited and no longer touch the database (the
    /// first call waits; later ones, from any clone, return at once).
    pub fn stop_and_join(&self) {
        self.stop();
        let threads = std::mem::take(&mut *lock(&self.threads));
        for t in threads {
            let _ = t.join();
        }
    }
}

#[cfg(test)]
impl Handle {
    /// A handle without threads: the test reads its jobs.
    pub(crate) fn detached(email: &str) -> (Handle, Receiver<Job>) {
        let (jobs, queue) = channel();
        let shared = Arc::new(Shared {
            email: email.into(), hub: Hub::default(), connect: Arc::new(|| Err("detached".into())), cache: PathBuf::new(),
            stop: Mutex::new(false), watch_kill: KillSlot::default(), work_kill: KillSlot::default(),
        });
        (Handle { email: email.into(), jobs, retry: channel().0, shared, threads: Arc::default() }, queue)
    }
}

pub(crate) type Connector = Arc<dyn Fn() -> Result<Conn, String> + Send + Sync>;

pub fn spawn(acc: Account, paths: Paths, hub: Hub) -> Handle {
    let a = acc.clone();
    spawn_with(acc, paths.db, paths.cache, hub, Arc::new(move || Conn::connect(&a, &secret::lookup(&a.email)?)))
}

pub(crate) fn spawn_with(acc: Account, db: PathBuf, cache: PathBuf, hub: Hub, connect: Connector) -> Handle {
    let (jobs, queue) = channel();
    let (retry, retries) = channel();
    let sh = Arc::new(Shared {
        email: acc.email.clone(), hub, connect, cache, stop: Mutex::new(false), watch_kill: KillSlot::default(), work_kill: KillSlot::default(),
    });
    let watcher = std::thread::spawn({
        let (sh, db, jobs) = (sh.clone(), db.clone(), jobs.clone());
        move || watch(&sh, &db, &jobs, &retries)
    });
    let worker = std::thread::spawn({
        let sh = sh.clone();
        move || {
            let r = catch_unwind(AssertUnwindSafe(|| work(&sh, &acc, &db, &queue))).unwrap_or_else(|_| Err("panicked".into()));
            if let Err(e) = r {
                eprintln!("{}: worker stopped: {e}", sh.email);
                sh.halt(Some(format!("worker stopped: {e}")));
            }
        }
    });
    Handle { email: sh.email.clone(), jobs, retry, shared: sh, threads: Arc::new(Mutex::new(vec![watcher, worker])) }
}

/// A panic (a bug, or the imap crate on an answer out of protocol) fails this one step only.
fn caught<T>(f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|_| Err("panicked".into()))
}

/// SyncFolder(Inbox), or SyncAll while the worker has not listed the folders yet.
fn inbox_sync(db: Option<&Db>, email: &str) -> Job {
    let inbox = db.and_then(|db| db.folders_of(email).ok()?.into_iter().find(|f| f.role == Role::Inbox));
    inbox.map_or(Job::SyncAll, |f| Job::SyncFolder(f.id))
}

fn watch(sh: &Shared, db: &Path, jobs: &Sender<Job>, retries: &Receiver<()>) {
    let db = Db::open(db).map_err(|e| eprintln!("{}: {e}", sh.email)).ok();
    // False: the worker is gone, nothing left to watch for.
    let send = |job: Job| {
        let sent = jobs.send(job).is_ok();
        if !sent {
            eprintln!("{}: worker stopped", sh.email);
            sh.status(Status::Offline("worker stopped".into()));
        }
        sent
    };
    if !sh.status(Status::Connecting) {
        return;
    }
    let (mut fails, mut first) = (0, true);
    loop {
        if fails > 0 {
            // `retry` and `stop` end the wait early.
            let _ = retries.recv_timeout(BACKOFF[(fails - 1).min(BACKOFF.len() - 1)]);
        }
        if sh.stopped() {
            return;
        }
        let mut conn = match caught(|| (sh.connect)()) {
            Ok(c) => c,
            Err(e) if e.starts_with(AUTH_FAILED) => {
                eprintln!("{}: {e}", sh.email);
                if !sh.status(Status::AuthFailed) {
                    return;
                }
                retries.try_iter().for_each(drop); // pokes from before the failure
                // No attempts on its own: repeated bad logins get accounts locked.
                if sh.stopped() || retries.recv().is_err() || !sh.status(Status::Connecting) {
                    return;
                }
                (fails, first) = (0, false);
                continue;
            }
            Err(e) => {
                if !sh.status(Status::Offline(e)) {
                    return;
                }
                (fails, first) = (fails + 1, false);
                continue;
            }
        };
        conn.set_kill_slot(sh.watch_kill.clone());
        // After the slot is set: a `stop` before it could not break this connection.
        if !sh.status(Status::Online) {
            return;
        }
        // Queued actions first, so that no sync before them can undo their effect locally. The
        // worker's start-up SyncAll covers the first session; after a break anything may have changed.
        if !send(Job::Actions) || (!first && !send(Job::SyncAll)) {
            return;
        }
        first = false;
        let since = Instant::now();
        let e = loop {
            match caught(|| conn.idle_inbox(IDLE_TIMEOUT)) {
                Ok(true) if !send(inbox_sync(db.as_ref(), &sh.email)) => return,
                Ok(_) => {}
                Err(e) => break e,
            }
            if sh.stopped() {
                return;
            }
        };
        if sh.stopped() {
            return;
        }
        eprintln!("{}: IDLE: {e}", sh.email);
        if !sh.status(Status::Connecting) {
            return;
        }
        fails = if since.elapsed() >= STEADY { 0 } else { fails + 1 };
    }
}

/// Runs `f` on the connection, opening it first if needed. A failed (or panicked) job drops the
/// connection and gets one more try on a fresh one, unless the account is stopping.
fn attempt<T>(sh: &Shared, conn: &mut Option<Conn>, mut f: impl FnMut(&mut Conn) -> Result<T, String>) -> Result<T, String> {
    let mut again = true;
    loop {
        let c = match conn {
            Some(c) => c,
            None => {
                let mut c = caught(|| (sh.connect)())?;
                c.set_kill_slot(sh.work_kill.clone());
                // After the slot is set: a `stop` before it could not break this connection.
                if sh.stopped() {
                    return Err("stopped".into());
                }
                conn.insert(c)
            }
        };
        match caught(|| f(c)) {
            Ok(v) => return Ok(v),
            Err(e) => {
                *conn = None;
                if !std::mem::take(&mut again) || sh.stopped() {
                    return Err(e);
                }
            }
        }
    }
}

/// Returns only when stopped (`Ok`) or on a fatal error.
fn work(sh: &Shared, acc: &Account, db: &Path, queue: &Receiver<Job>) -> Result<(), String> {
    let db = Db::open(db)?;
    let mut conn = None;
    let mut auth_failed = false;
    let mut next = Some(Job::SyncAll);
    let mut due = Instant::now() + PERIOD;
    loop {
        let job = match next.take() {
            Some(j) => j,
            None => match queue.recv_timeout(due.saturating_duration_since(Instant::now())) {
                Ok(j) => j,
                // No attempts on its own after a rejected login; a SyncAll job tries again.
                Err(RecvTimeoutError::Timeout) if auth_failed => {
                    due = Instant::now() + PERIOD;
                    continue;
                }
                // Actions too: one that failed on the worker's connection waits for no reconnect.
                Err(RecvTimeoutError::Timeout) => {
                    next = Some(Job::SyncAll);
                    Job::Actions
                }
                Err(RecvTimeoutError::Disconnected) => return Ok(()),
            },
        };
        if sh.stopped() {
            return Ok(());
        }
        let r = match job {
            Job::SyncAll => {
                due = Instant::now() + PERIOD;
                if acc.append_sent && !auth_failed && let Some(f) = db.folders_of(&acc.email).ok().and_then(|v| v.into_iter().find(|f| f.role == Role::Sent)) {
                    let _ = smtp::flush_unfiled(&db, &acc.email, |raw| attempt(sh, &mut conn, |c| c.append(&f.path, raw).map_err(String::from)));
                }
                attempt(sh, &mut conn, |c| sync::sync_account(c, &db, acc))
            }
            Job::SyncFolder(id) => attempt(sh, &mut conn, |c| match db.folders_of(&acc.email)?.into_iter().find(|f| f.id == id) {
                Some(f) => sync::sync_folder(c, &db, &f),
                None => Ok(vec![]),
            }),
            // Nothing queued: no connection. After a rejected login: no new attempts.
            Job::Actions if auth_failed || matches!(actions::has_pending(&db, &acc.email), Ok(false)) => continue,
            Job::Actions => attempt(sh, &mut conn, |c| actions::run(c, &db, &acc.email, &sh.hub)).map(|()| vec![]),
            Job::Fetch { message, reply } => {
                let r = attempt(sh, &mut conn, |c| fetch::fetch_body(c, &db, &sh.cache, message));
                // A background fetch has no one waiting: the window learns from the event.
                if matches!(r, Ok(true)) {
                    sh.hub.emit(Event::Changed { folders: false, ids: vec![message] });
                }
                let _ = reply.send(r.map(drop));
                continue;
            }
            Job::FetchRemote { folder_path, uid, reply } => {
                let _ = reply.send(attempt(sh, &mut conn, |c| search::open(c, &db, &acc.email, &folder_path, uid)));
                continue;
            }
            Job::Attachment { message, index, reply } => {
                let _ = reply.send(attempt(sh, &mut conn, |c| fetch::fetch_attachment(c, &db, &sh.cache, message, index)));
                continue;
            }
            Job::Search { mut query, mut token } => {
                // A newer search of the window makes the queued older ones pointless.
                while next.is_none() {
                    match queue.try_recv() {
                        Ok(Job::Search { query: q, token: t }) => (query, token) = (q, t),
                        Ok(other) => next = Some(other),
                        Err(_) => break,
                    }
                }
                let r = attempt(sh, &mut conn, |c| search::search(c, acc, &query));
                // Also without hits or on failure: the window stops its spinner on the event.
                let (hits, error) = match r {
                    Ok(r) => r,
                    Err(e) => (vec![], Some(e)),
                };
                sh.hub.emit(Event::SearchRemote { token, hits, error });
                continue;
            }
            // The copy for servers that do not file sent mail themselves; then the folder is synced.
            // A failed APPEND (offline) keeps the row; flush_unfiled retries it (the mail itself already went out).
            Job::Append { outbox, role, raw } => {
                let Some(f) = db.folders_of(&acc.email).ok().and_then(|v| v.into_iter().find(|f| f.role == role)) else { continue };
                // A failure keeps the row (state `sent`); the next SyncAll files it.
                match smtp::append_row(&db, outbox, &raw, |raw| attempt(sh, &mut conn, |c| c.append(&f.path, raw).map_err(String::from))) {
                    Ok(()) => next = Some(Job::SyncFolder(f.id)),
                    Err(e) => eprintln!("{}: append to {}: {e}", acc.email, f.path),
                }
                continue;
            }
        };
        auth_failed = matches!(&r, Err(e) if e.starts_with(AUTH_FAILED));
        if r.is_ok() {
            // Folders are re-read: the sync has just moved `notified_uid` and `last_uid`.
            if let Some(inbox) = db.folders_of(&acc.email).ok().and_then(|v| v.into_iter().find(|f| f.role == Role::Inbox)) {
                notify::announce(&db, &sh.hub, &inbox);
            }
        }
        match r {
            // Also with no new mail: flag changes and deletions (from a phone too) insert nothing.
            Ok(ids) => sh.hub.emit(Event::Changed { folders: true, ids }),
            Err(e) => eprintln!("{}: {e}", acc.email),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Provider, Server};
    use crate::imap::fake_server;
    use std::os::unix::fs::PermissionsExt;
    use std::net::{SocketAddr, TcpStream};
    use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};

    fn acc() -> Account {
        let s = Server { host: "h".into(), port: 1 };
        Account { email: "me@x".into(), name: "N".into(), provider: Provider::Imap, login: "me".into(), imap: s.clone(), smtp: s, append_sent: false }
    }

    /// Threads over the fake server, with the database in a fresh temp dir (removed by the caller).
    fn start(addr: SocketAddr, name: &str, hub: &Hub) -> (Handle, PathBuf) {
        let dir = std::env::temp_dir().join(format!("maild-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        (spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), hub.clone(), Arc::new(connect)), dir)
    }

    fn until(what: &str, ok: impl Fn() -> bool) {
        let end = Instant::now() + Duration::from_secs(2);
        while !ok() {
            assert!(Instant::now() < end, "timed out: {what}");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn count(n: &AtomicUsize) -> usize { n.load(SeqCst) }

    #[test]
    fn watcher_reconnects_after_kill() {
        let (idles, lists) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let (i, l) = (idles.clone(), lists.clone());
        let addr = fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => { l.fetch_add(1, SeqCst); "* LIST () \"/\" \"INBOX\"\r\n".into() }
            "EXAMINE \"INBOX\"" => "* 0 EXISTS\r\n* OK [UIDVALIDITY 1] v\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH\r\n".into(),
            // the second session's IDLE reports an update
            "IDLE" if i.fetch_add(1, SeqCst) == 1 => "* 1 EXISTS\r\n".into(),
            "IDLE" => String::new(),
            _ => return None,
        }));
        let hub = Hub::default();
        let events = hub.subscribe(false);
        let (h, dir) = start(addr, "watch", &hub);
        let db = Db::open(&dir.join("mail.db")).unwrap();
        until("start-up SyncAll and IDLE", || count(&idles) == 1 && !db.folders_of("me@x").unwrap().is_empty());

        h.interrupt();
        until("second IDLE", || count(&idles) >= 2);
        until("SyncAll after reconnect", || count(&lists) >= 2);
        // the update → SyncFolder(inbox) → changed
        let end = Instant::now() + Duration::from_secs(2);
        while !events.recv_timeout(end.saturating_duration_since(Instant::now())).unwrap().starts_with(r#"{"event":"changed""#) {}
        assert_eq!(hub.statuses()[0].status, Status::Online);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn panic_fails_one_step_only() {
        thread_local!(static PANICKED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) });
        let (idles, lists) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let (i, l) = (idles.clone(), lists.clone());
        let addr = fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => { l.fetch_add(1, SeqCst); "* LIST () \"/\" \"INBOX\"\r\n".into() }
            "EXAMINE \"INBOX\"" => "* 0 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH\r\n".into(),
            "IDLE" => { i.fetch_add(1, SeqCst); String::new() }
            _ => return None,
        }));
        let dir = std::env::temp_dir().join(format!("maild-test-{}-panic", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        // the first connect of each thread panics (the watcher's session, the worker's start-up SyncAll)
        let connect = move || {
            if !PANICKED.replace(true) {
                panic!("test panic");
            }
            Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw")
        };
        let hub = Hub::default();
        let h = spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), hub.clone(), Arc::new(connect));
        // the watcher reconnects and asks for a SyncAll; the worker runs it
        until("next session and next job", || count(&idles) >= 1 && count(&lists) >= 1);
        h.jobs.send(Job::SyncAll).unwrap();
        until("one more job", || count(&lists) >= 2);
        assert_eq!(hub.statuses()[0].status, Status::Online);
        h.stop_and_join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn search_job_emits_event_per_token() {
        let addr = fake_server(|cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 0 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH\r\n".into(),
            "UID SEARCH BEFORE 1-Jan-2026 OR OR SUBJECT \"x\" FROM \"x\" TEXT \"x\"" => "* SEARCH 3\r\n".into(),
            "UID FETCH 3 (UID FLAGS INTERNALDATE BODY.PEEK[HEADER])" => {
                "* 1 FETCH (UID 3 FLAGS () BODY[HEADER] {16}\r\nSubject: old\r\n\r\n)\r\n".into()
            }
            "IDLE" => String::new(),
            _ => return None,
        }));
        let hub = Hub::default();
        let events = hub.subscribe(false);
        let (h, dir) = start(addr, "search", &hub);
        h.jobs.send(Job::Search { query: "x".into(), token: 7 }).unwrap();
        let end = Instant::now() + Duration::from_secs(2);
        let ev = loop {
            let e = events.recv_timeout(end.saturating_duration_since(Instant::now())).unwrap();
            if e.starts_with(r#"{"event":"search_remote""#) { break e }
        };
        assert!(ev.contains(r#""token":7"#) && ev.contains(r#""subject":"old""#) && ev.contains(r#""uid":3"#), "{ev}");
        h.stop_and_join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn auth_failure_waits_for_retry() {
        let logins = Arc::new(AtomicUsize::new(0));
        let n = logins.clone();
        let addr = fake_server(move |cmd| cmd.starts_with("LOGIN").then(|| {
            n.fetch_add(1, SeqCst);
            "NO [AUTHENTICATIONFAILED] Invalid credentials".into()
        }));
        let hub = Hub::default();
        let (h, dir) = start(addr, "auth", &hub);
        // one attempt each: the watcher and the worker's start-up SyncAll
        until("AuthFailed", || count(&logins) == 2 && hub.statuses().first().is_some_and(|a| a.status == Status::AuthFailed));
        std::thread::sleep(BACKOFF[2] * 4);
        assert_eq!(count(&logins), 2, "retried on its own");

        h.retry();
        until("retry", || count(&logins) == 4);
        let _ = std::fs::remove_dir_all(dir);
    }

    /// Logs in (counted), lists INBOX, idles silently.
    fn quiet_server(logins: Arc<AtomicUsize>) -> SocketAddr {
        fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => { logins.fetch_add(1, SeqCst); String::new() }
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 0 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH\r\n".into(),
            "IDLE" => String::new(),
            _ => return None,
        }))
    }

    #[test]
    fn remove_handle_stops_and_joins() {
        let logins = Arc::new(AtomicUsize::new(0));
        let hub = Hub::default();
        let (h, dir) = start(quiet_server(logins.clone()), "stop", &hub);
        hub.add_handle(h);
        until("both threads logged in", || count(&logins) == 2 && hub.statuses().first().is_some_and(|a| a.status == Status::Online));

        let t = Instant::now();
        hub.remove_handle("me@x");
        assert!(t.elapsed() < Duration::from_secs(2), "{:?}", t.elapsed());
        assert!(hub.handles().is_empty() && hub.statuses().is_empty());
        std::thread::sleep(BACKOFF[2] * 4);
        assert_eq!(count(&logins), 2, "reconnected after stop");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn pending_action_runs_on_connect() {
        let dir = std::env::temp_dir().join(format!("maild-test-{}-pending", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let db = Db::open(&dir.join("mail.db")).unwrap();
        let inbox = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let id = db.insert_message(inbox, 5, false, false, 1, &crate::mime::Parsed::default(), true).unwrap();
        // queued before the start, e.g. offline before a restart
        crate::actions::enqueue(&db, &[id], crate::actions::Op::Read).unwrap();
        let stores = Arc::new(AtomicUsize::new(0));
        let n = stores.clone();
        let addr = fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 1 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 5\r\n".into(),
            // the server does not have the read yet: the start-up sync must not undo it locally
            "UID FETCH 5:* (UID FLAGS)" => "* 1 FETCH (UID 5 FLAGS ())\r\n".into(),
            "SELECT \"INBOX\"" | "IDLE" => String::new(),
            "UID STORE 5 +FLAGS.SILENT (\\Seen)" => { n.fetch_add(1, SeqCst); String::new() }
            _ => return None,
        }));
        let hub = Hub::default();
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), hub.clone(), Arc::new(connect));
        until("the queued read reached the server", || count(&stores) == 1 && db.queued_count("me@x").unwrap() == 0);
        assert!(db.message(id).unwrap().unwrap().summary.seen);
        h.stop_and_join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn dead_worker_is_visible() {
        let hub = Hub::default();
        let dir = std::env::temp_dir().join(format!("maild-test-{}-dead", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        // a directory where the database file should be: the worker's Db::open fails
        std::fs::create_dir_all(dir.join("mail.db")).unwrap();
        let addr = quiet_server(Arc::new(AtomicUsize::new(0)));
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), hub.clone(), Arc::new(connect));
        let dead = || hub.statuses().first().is_some_and(|a| matches!(&a.status, Status::Offline(e) if e.starts_with("worker stopped")));
        until("worker stopped", dead);
        h.stop_and_join();
        assert!(dead());
        let _ = std::fs::remove_dir_all(dir);
    }

    const RAW: &str = "From: a@x\r\nSubject: s\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=B\r\n\r\n\
--B\r\nContent-Type: text/html\r\n\r\n<p>hi</p><img src=\"cid:logo@x\">\r\n\
--B\r\nContent-Type: image/png\r\nContent-ID: <logo@x>\r\nContent-Transfer-Encoding: base64\r\n\r\naW1n\r\n\
--B\r\nContent-Type: application/pdf; name=\"../a.pdf\"\r\nContent-Disposition: attachment; filename=\"../a.pdf\"\r\nContent-Transfer-Encoding: base64\r\n\r\ncGRm\r\n--B--\r\n";

    /// A server with one message (uid 5) that serves `RAW`; counts full fetches.
    fn mail_server(fetches: Arc<AtomicUsize>) -> SocketAddr {
        fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 1 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 5\r\n".into(),
            "UID FETCH 5:* (UID FLAGS)" => "* 1 FETCH (UID 5 FLAGS ())\r\n".into(),
            "UID FETCH 5 (UID BODY.PEEK[])" => {
                fetches.fetch_add(1, SeqCst);
                format!("* 1 FETCH (UID 5 BODY[] {{{}}}\r\n{RAW})\r\n", RAW.len())
            }
            "SELECT \"INBOX\"" | "IDLE" => String::new(),
            _ => return None,
        }))
    }

    #[test]
    fn fetch_and_attachment_jobs() {
        let dir = std::env::temp_dir().join(format!("maild-test-{}-fetch", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let db = Db::open(&dir.join("mail.db")).unwrap();
        let inbox = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        // as a big message is stored: no body, attachment list from BODYSTRUCTURE with a shifted index
        let mut p = crate::mime::Parsed::default();
        let a = |index, name: &str, mime: &str, cid: Option<&str>| crate::mime::Attachment { index, name: name.into(), mime: mime.into(), size: 1, cid: cid.map(Into::into), inline: cid.is_some() };
        p.attachments = vec![a(0, "../a.pdf", "application/pdf", None), a(1, "logo", "image/png", Some("logo@x"))];
        let id = db.insert_message(inbox, 5, false, false, 3 << 20, &p, false).unwrap();
        let fetches = Arc::new(AtomicUsize::new(0));
        let addr = mail_server(fetches.clone());
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), Hub::default(), Arc::new(connect));

        let (tx, rx) = channel();
        h.jobs.send(Job::Fetch { message: id, reply: tx }).unwrap();
        rx.recv_timeout(Duration::from_secs(2)).unwrap().unwrap();
        let f = db.message(id).unwrap().unwrap();
        assert!(f.has_body && f.html.unwrap().contains("hi"));
        // the authoritative list: the image comes first in the message
        assert_eq!((f.attachments[0].name.as_str(), f.attachments[0].cid.as_deref()), ("attachment", Some("logo@x")));
        let img = dir.join("cache").join(id.to_string()).join("cid-0");
        assert_eq!(std::fs::read(&img).unwrap(), b"img");
        assert_eq!(std::fs::metadata(&img).unwrap().permissions().mode() & 0o777, 0o600);
        assert_eq!(std::fs::metadata(img.parent().unwrap()).unwrap().permissions().mode() & 0o777, 0o700);

        // the stored list was replaced: the pdf is index 1 now
        let (tx, rx) = channel();
        h.jobs.send(Job::Attachment { message: id, index: 1, reply: tx }).unwrap();
        let path = rx.recv_timeout(Duration::from_secs(2)).unwrap().unwrap();
        assert_eq!(path, dir.join("cache").join(id.to_string()).join("1").join("a.pdf"));
        assert_eq!(std::fs::read(&path).unwrap(), b"pdf");
        h.stop_and_join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn attachment_found_by_name_not_index() {
        let dir = std::env::temp_dir().join(format!("maild-test-{}-byname", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let db = Db::open(&dir.join("mail.db")).unwrap();
        let inbox = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let mut p = crate::mime::Parsed::default();
        // BODYSTRUCTURE said the pdf is index 0; mail-parser has it at 1
        p.attachments = vec![crate::mime::Attachment { index: 0, name: "../a.pdf".into(), mime: "application/pdf".into(), size: 1, cid: None, inline: false }];
        let id = db.insert_message(inbox, 5, false, false, 3 << 20, &p, true).unwrap();
        let addr = mail_server(Arc::new(AtomicUsize::new(0)));
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), Hub::default(), Arc::new(connect));
        let (tx, rx) = channel();
        h.jobs.send(Job::Attachment { message: id, index: 0, reply: tx }).unwrap();
        let path = rx.recv_timeout(Duration::from_secs(2)).unwrap().unwrap();
        assert_eq!(std::fs::read(path).unwrap(), b"pdf");
        assert_eq!(db.message(id).unwrap().unwrap().attachments[1].index, 1, "stored list is mail-parser's now");
        h.stop_and_join();
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn same_name_attachments_do_not_collide() {
        const TWO: &str = "From: a@x\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=B\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\nhi\r\n\
--B\r\nContent-Type: application/pdf; name=\"scan.pdf\"\r\nContent-Disposition: attachment; filename=\"scan.pdf\"\r\nContent-Transfer-Encoding: base64\r\n\r\nb25l\r\n\
--B\r\nContent-Type: application/pdf; name=\"scan.pdf\"\r\nContent-Disposition: attachment; filename=\"scan.pdf\"\r\nContent-Transfer-Encoding: base64\r\n\r\ndHdv\r\n--B--\r\n";
        let dir = std::env::temp_dir().join(format!("maild-test-{}-same", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let db = Db::open(&dir.join("mail.db")).unwrap();
        let inbox = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let mut p = crate::mime::Parsed::default();
        p.attachments = (0..2).map(|index| crate::mime::Attachment { index, name: "scan.pdf".into(), mime: "application/pdf".into(), size: 3, cid: None, inline: false }).collect();
        let id = db.insert_message(inbox, 5, false, false, 3 << 20, &p, true).unwrap();
        let addr = fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 1 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 5\r\n".into(),
            "UID FETCH 5:* (UID FLAGS)" => "* 1 FETCH (UID 5 FLAGS ())\r\n".into(),
            "UID FETCH 5 (UID BODY.PEEK[])" => format!("* 1 FETCH (UID 5 BODY[] {{{}}}\r\n{TWO})\r\n", TWO.len()),
            "SELECT \"INBOX\"" | "IDLE" => String::new(),
            _ => return None,
        }));
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = spawn_with(acc(), dir.join("mail.db"), dir.join("cache"), Hub::default(), Arc::new(connect));
        let mut got = vec![];
        for index in 0..2 {
            let (tx, rx) = channel();
            h.jobs.send(Job::Attachment { message: id, index, reply: tx }).unwrap();
            let path = rx.recv_timeout(Duration::from_secs(2)).unwrap().unwrap();
            assert_eq!(path.file_name().unwrap(), "scan.pdf");
            got.push((std::fs::read(&path).unwrap(), path));
        }
        assert_eq!((got[0].0.as_slice(), got[1].0.as_slice()), (&b"one"[..], &b"two"[..]));
        assert_ne!(got[0].1, got[1].1);
        h.stop_and_join();
        let _ = std::fs::remove_dir_all(dir);
    }
}
