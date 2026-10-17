//! Per-account threads, each with its own connection and `Db`: the watcher (IDLE on Inbox, says
//! when something came) and the worker (the job queue: syncs, and later actions, fetches, search).

use crate::config::{Account, Paths};
use crate::db::{Db, Role};
use crate::hub::{Event, Hub};
use crate::imap::{self, AUTH_FAILED, Conn, KillSlot};
use crate::{secret, sync};
use serde::Serialize;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender, TryRecvError, channel};
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

/// Both threads stop once every clone is dropped (`interrupt` makes an idling watcher notice).
#[derive(Clone)]
pub struct Handle {
    pub email: String,
    pub jobs: Sender<Job>,
    /// The watcher's socket.
    pub kill: KillSlot,
    work_kill: KillSlot,
    retry: Sender<()>,
}

impl Handle {
    /// Breaks both connections (after sleep): blocked reads end at once, the threads reconnect.
    pub fn interrupt(&self) {
        imap::kill(&self.kill);
        imap::kill(&self.work_kill);
    }

    /// "Sync now": queues SyncAll and wakes a watcher that waits out AuthFailed or a backoff.
    pub fn retry(&self) {
        let _ = self.retry.send(());
        let _ = self.jobs.send(Job::SyncAll);
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
    let h = Handle { email: acc.email.clone(), jobs, kill: KillSlot::default(), work_kill: KillSlot::default(), retry };
    std::thread::spawn({
        let (email, db, hub, connect, jobs, kill) = (acc.email.clone(), db.clone(), hub.clone(), connect.clone(), h.jobs.clone(), h.kill.clone());
        move || watch(email, db, hub, connect, jobs, retries, kill)
    });
    let kill = h.work_kill.clone();
    std::thread::spawn(move || work(acc, db, hub, connect, queue, kill));
    h
}

/// Drains retry pokes; true once every Handle is dropped.
fn gone(retries: &Receiver<()>) -> bool {
    retries.try_iter().for_each(drop);
    retries.try_recv() == Err(TryRecvError::Disconnected)
}

/// SyncFolder(Inbox), or SyncAll while the worker has not listed the folders yet.
fn inbox_sync(db: Option<&Db>, email: &str) -> Job {
    let inbox = db.and_then(|db| db.folders_of(email).ok()?.into_iter().find(|f| f.role == Role::Inbox));
    inbox.map_or(Job::SyncAll, |f| Job::SyncFolder(f.id))
}

fn watch(email: String, db: PathBuf, hub: Hub, connect: Connector, jobs: Sender<Job>, retries: Receiver<()>, kill: KillSlot) {
    let db = Db::open(&db).map_err(|e| eprintln!("{email}: {e}")).ok();
    hub.set_status(&email, Status::Connecting);
    let (mut fails, mut first) = (0, true);
    loop {
        if fails > 0 && retries.recv_timeout(BACKOFF[(fails - 1).min(BACKOFF.len() - 1)]) == Err(RecvTimeoutError::Disconnected) {
            return;
        }
        let mut conn = match connect() {
            Ok(c) => c,
            Err(e) if e.starts_with(AUTH_FAILED) => {
                eprintln!("{email}: {e}");
                hub.set_status(&email, Status::AuthFailed);
                // No attempts on its own: repeated bad logins get accounts locked.
                if retries.recv().is_err() {
                    return;
                }
                (fails, first) = (0, false);
                hub.set_status(&email, Status::Connecting);
                continue;
            }
            Err(e) => {
                hub.set_status(&email, Status::Offline(e));
                (fails, first) = (fails + 1, false);
                continue;
            }
        };
        conn.set_kill_slot(kill.clone());
        hub.set_status(&email, Status::Online);
        // The worker's start-up SyncAll covers the first session; after a break anything may have changed.
        if !first {
            let _ = jobs.send(Job::SyncAll);
        }
        first = false;
        let since = Instant::now();
        let e = loop {
            match conn.idle_inbox(IDLE_TIMEOUT) {
                Ok(true) => { let _ = jobs.send(inbox_sync(db.as_ref(), &email)); }
                Ok(false) => {}
                Err(e) => break e,
            }
            if gone(&retries) {
                return;
            }
        };
        if gone(&retries) {
            return;
        }
        eprintln!("{email}: IDLE: {e}");
        hub.set_status(&email, Status::Connecting);
        fails = if since.elapsed() >= STEADY { 0 } else { fails + 1 };
    }
}

/// Runs `f` on the connection, opening it first if needed. A failed job drops the connection and
/// gets one more try on a fresh one.
fn attempt<T>(conn: &mut Option<Conn>, open: &dyn Fn() -> Result<Conn, String>, mut f: impl FnMut(&mut Conn) -> Result<T, String>) -> Result<T, String> {
    let mut again = true;
    loop {
        let c = match conn {
            Some(c) => c,
            None => conn.insert(open()?),
        };
        match f(c) {
            Ok(v) => return Ok(v),
            Err(e) => {
                *conn = None;
                if !std::mem::take(&mut again) {
                    return Err(e);
                }
            }
        }
    }
}

fn work(acc: Account, db: PathBuf, hub: Hub, connect: Connector, queue: Receiver<Job>, kill: KillSlot) {
    let db = match Db::open(&db) {
        Ok(db) => db,
        Err(e) => return eprintln!("{}: {e}", acc.email),
    };
    let open = || connect().map(|mut c| {
        c.set_kill_slot(kill.clone());
        c
    });
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
                Err(RecvTimeoutError::Disconnected) => return,
            },
        };
        let (r, always) = match job {
            Job::SyncAll => {
                due = Instant::now() + PERIOD;
                (attempt(&mut conn, &open, |c| sync::sync_account(c, &db, &acc)), false)
            }
            // Flag changes and deletions insert nothing, but the window still has to refresh.
            Job::SyncFolder(id) => (attempt(&mut conn, &open, |c| match db.folders_of(&acc.email)?.into_iter().find(|f| f.id == id) {
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
        match r {
            Ok(ids) if always || !ids.is_empty() => hub.emit(Event::Changed { folders: true, ids }),
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
}
