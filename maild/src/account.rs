//! Per-account threads, each with its own connection and `Db`: the watcher (IDLE on Inbox, says
//! when something came) and the worker (the job queue: syncs, and later actions, fetches, search).

use crate::config::{Account, Paths};
use crate::db::{Db, Role};
use crate::hub::{Event, Hub, lock};
use crate::imap::{self, AUTH_FAILED, Conn, KillSlot};
use crate::{notify, secret, sync};
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
const NOT_IMPLEMENTED: &str = "not implemented";

/// Worker jobs. `Actions`, `Fetch*`, `Attachment`, `Search`, `Append` are filled in by tasks 8–11.
pub enum Job {
    SyncFolder(i64),
    SyncAll,
    Actions,
    Fetch { message: i64, reply: Sender<Result<(), String>> },
    FetchRemote { folder_path: String, uid: u32, reply: Sender<Result<i64, String>> },
    Attachment { message: i64, index: u32, reply: Sender<Result<PathBuf, String>> },
    Search { query: String, token: u64 },
    Append { role: Role, raw: Vec<u8> },
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
    /// The watcher's socket.
    pub kill: KillSlot,
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

    /// "Sync now": queues SyncAll and wakes a watcher that waits out AuthFailed or a backoff.
    pub fn retry(&self) {
        let _ = self.retry.send(());
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

type Connector = Arc<dyn Fn() -> Result<Conn, String> + Send + Sync>;

pub fn spawn(acc: Account, paths: Paths, hub: Hub) -> Handle {
    let a = acc.clone();
    spawn_with(acc, paths.db, hub, Arc::new(move || Conn::connect(&a, &secret::lookup(&a.email)?)))
}

fn spawn_with(acc: Account, db: PathBuf, hub: Hub, connect: Connector) -> Handle {
    let (jobs, queue) = channel();
    let (retry, retries) = channel();
    let sh = Arc::new(Shared {
        email: acc.email.clone(), hub, connect, stop: Mutex::new(false), watch_kill: KillSlot::default(), work_kill: KillSlot::default(),
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
    Handle { email: sh.email.clone(), jobs, kill: sh.watch_kill.clone(), retry, shared: sh, threads: Arc::new(Mutex::new(vec![watcher, worker])) }
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
        let mut conn = match (sh.connect)() {
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
        // The worker's start-up SyncAll covers the first session; after a break anything may have changed.
        if !first && !send(Job::SyncAll) {
            return;
        }
        first = false;
        let since = Instant::now();
        let e = loop {
            match conn.idle_inbox(IDLE_TIMEOUT) {
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

/// Runs `f` on the connection, opening it first if needed. A failed job drops the connection and
/// gets one more try on a fresh one, unless the account is stopping.
fn attempt<T>(sh: &Shared, conn: &mut Option<Conn>, mut f: impl FnMut(&mut Conn) -> Result<T, String>) -> Result<T, String> {
    let mut again = true;
    loop {
        let c = match conn {
            Some(c) => c,
            None => {
                let mut c = (sh.connect)()?;
                c.set_kill_slot(sh.work_kill.clone());
                // After the slot is set: a `stop` before it could not break this connection.
                if sh.stopped() {
                    return Err("stopped".into());
                }
                conn.insert(c)
            }
        };
        match f(c) {
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
                Err(RecvTimeoutError::Timeout) => Job::SyncAll,
                Err(RecvTimeoutError::Disconnected) => return Ok(()),
            },
        };
        if sh.stopped() {
            return Ok(());
        }
        let (r, always) = match job {
            Job::SyncAll => {
                due = Instant::now() + PERIOD;
                (attempt(sh, &mut conn, |c| sync::sync_account(c, &db, acc)), false)
            }
            // Flag changes and deletions insert nothing, but the window still has to refresh.
            Job::SyncFolder(id) => (attempt(sh, &mut conn, |c| match db.folders_of(&acc.email)?.into_iter().find(|f| f.id == id) {
                Some(f) => sync::sync_folder(c, &db, &f),
                None => Ok(vec![]),
            }), true),
            Job::Fetch { reply, .. } => {
                let _ = reply.send(Err(NOT_IMPLEMENTED.into()));
                continue;
            }
            Job::FetchRemote { reply, .. } => {
                let _ = reply.send(Err(NOT_IMPLEMENTED.into()));
                continue;
            }
            Job::Attachment { reply, .. } => {
                let _ = reply.send(Err(NOT_IMPLEMENTED.into()));
                continue;
            }
            Job::Actions | Job::Search { .. } | Job::Append { .. } => continue,
        };
        auth_failed = matches!(&r, Err(e) if e.starts_with(AUTH_FAILED));
        if r.is_ok() {
            // Folders are re-read: the sync has just moved `notified_uid` and `last_uid`.
            if let Some(inbox) = db.folders_of(&acc.email).ok().and_then(|v| v.into_iter().find(|f| f.role == Role::Inbox)) {
                notify::announce(&db, &sh.hub, &inbox);
            }
        }
        match r {
            Ok(ids) if always || !ids.is_empty() => sh.hub.emit(Event::Changed { folders: true, ids }),
            Ok(_) => {}
            Err(e) => eprintln!("{}: {e}", acc.email),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Provider, Server};
    use crate::imap::fake_server;
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
        (spawn_with(acc(), dir.join("mail.db"), hub.clone(), Arc::new(connect)), dir)
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
    fn dead_worker_is_visible() {
        let hub = Hub::default();
        let dir = std::env::temp_dir().join(format!("maild-test-{}-dead", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        // a directory where the database file should be: the worker's Db::open fails
        std::fs::create_dir_all(dir.join("mail.db")).unwrap();
        let addr = quiet_server(Arc::new(AtomicUsize::new(0)));
        let connect = move || Conn::plain(TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = spawn_with(acc(), dir.join("mail.db"), hub.clone(), Arc::new(connect));
        let dead = || hub.statuses().first().is_some_and(|a| matches!(&a.status, Status::Offline(e) if e.starts_with("worker stopped")));
        until("worker stopped", dead);
        h.stop_and_join();
        assert!(dead());
        let _ = std::fs::remove_dir_all(dir);
    }
}
