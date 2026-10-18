//! New-mail notifications: what to show (`plan`), showing it, and what a click does.

use crate::db::{Db, Folder, Summary};
use crate::hub::{Event, Hub, lock};
use crate::i18n;
use notify_rust::{Hint, Notification};
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

/// Up to this many new messages get a notification each; more are folded into one.
const EACH: usize = 3;
const DESKTOP_ID: &str = "omarchy-mail";
/// The notification server's bus name, also its interface.
const SERVER: &str = "org.freedesktop.Notifications";
const SERVER_PATH: &str = "/org/freedesktop/Notifications";
/// Clickable notifications remembered; older ones (still in the center) open nothing.
const KEEP: usize = 1000;

static SHOWN: Mutex<Shown> = Mutex::new(Shown { owner: String::new(), ids: VecDeque::new(), file: None, resumed: false });

/// The shown notifications that open a message. Saved to `file` on every change, so the
/// ones still in the notification center stay clickable after a maild restart.
#[derive(Default, Serialize, Deserialize)]
struct Shown {
    /// Unique bus name of the notification server the ids belong to; empty: none.
    owner: String,
    /// (server notification id, message id), oldest first.
    ids: VecDeque<(u32, i64)>,
    #[serde(skip)]
    file: Option<PathBuf>,
    /// `owner` is known; until then nothing is saved, the previous maild's file is still to be read.
    #[serde(skip)]
    resumed: bool,
}

impl Shown {
    fn record(&mut self, id: u32, msg: i64) {
        if self.ids.len() >= KEEP {
            self.ids.pop_front();
        }
        self.ids.push_back((id, msg));
        self.save();
    }

    /// The message to open for `ActionInvoked(id, key)`.
    fn clicked(&self, id: u32, key: &str) -> Option<i64> {
        if key != "default" {
            return None;
        }
        self.ids.iter().rev().find(|e| e.0 == id).map(|e| e.1)
    }

    fn closed(&mut self, id: u32) {
        let n = self.ids.len();
        self.ids.retain(|e| e.0 != id);
        if self.ids.len() != n {
            self.save();
        }
    }

    /// A restarted server numbers from 1 again: the old ids now belong to other apps.
    fn owner_changed(&mut self, owner: &str) {
        self.owner = owner.into();
        self.ids.clear();
        self.save();
    }

    /// On (re)connecting, with the server running now: keeps its ids, from memory or from
    /// the file a previous maild left; drops those of any other server.
    fn resume(&mut self, owner: &str) {
        if self.resumed && self.owner == owner {
            return;
        }
        let saved = self.file.as_deref().and_then(|f| serde_json::from_slice::<Shown>(&std::fs::read(f).ok()?).ok());
        let mut ids = saved.filter(|s| s.owner == owner).map(|s| s.ids).unwrap_or_default();
        // Recorded before the first resume: shown by the server running now.
        if !self.resumed {
            ids.append(&mut self.ids);
        }
        ids.drain(..ids.len().saturating_sub(KEEP));
        self.ids = ids;
        self.owner = owner.into();
        self.resumed = true;
        self.save();
    }

    /// Atomic, the file 0600 in a 0700 directory. Failures are logged: clicks still work until a restart.
    fn save(&self) {
        let Some(path) = self.file.as_ref().filter(|_| self.resumed) else { return };
        let (Some(dir), Some(name)) = (path.parent(), path.file_name().and_then(|n| n.to_str())) else { return };
        let saved = serde_json::to_vec(self).map_err(|e| e.to_string()).and_then(|b| crate::fetch::store(dir, name, &b));
        if let Err(e) = saved {
            eprintln!("notify: {}: {e}", path.display());
        }
    }

    /// Applies a signal of the notification server or the bus; returns the message to open.
    fn on_signal(&mut self, msg: &zbus::Message) -> Option<i64> {
        let h = msg.header();
        let from_server = h.path().is_some_and(|p| p.as_str() == SERVER_PATH);
        match (h.interface()?.as_str(), h.member()?.as_str()) {
            (SERVER, "ActionInvoked") if from_server => {
                let (id, key): (u32, String) = msg.body().deserialize().ok()?;
                self.clicked(id, &key)
            }
            (SERVER, "NotificationClosed") if from_server => {
                let (id, _reason): (u32, u32) = msg.body().deserialize().ok()?;
                self.closed(id);
                None
            }
            ("org.freedesktop.DBus", "NameOwnerChanged") => {
                let (name, _, owner): (String, String, String) = msg.body().deserialize().ok()?;
                if name == SERVER {
                    self.owner_changed(&owner);
                }
                None
            }
            _ => None,
        }
    }
}

/// Starts listening for clicks, for the service's lifetime. The ids a previous maild saved in
/// `file` stay clickable while the same notification server runs.
pub fn start(hub: Hub, file: PathBuf) {
    lock(&SHOWN).file = Some(file);
    std::thread::spawn(move || listen(hub));
}

/// One session bus connection for every notification's clicks; reconnects.
fn listen(hub: Hub) {
    loop {
        if let Err(e) = zbus::blocking::Connection::session().and_then(|c| listen_on(c, &hub, &SHOWN)) {
            eprintln!("notify: click listener: {e}");
        }
        std::thread::sleep(Duration::from_secs(5));
    }
}

/// Returns when the connection drops.
fn listen_on(conn: zbus::blocking::Connection, hub: &Hub, shown: &Mutex<Shown>) -> zbus::Result<()> {
    use zbus::{MatchRule, message::Type::Signal};
    let signals = zbus::blocking::MessageIterator::from(conn.clone());
    let dbus = zbus::blocking::fdo::DBusProxy::new(&conn)?;
    dbus.add_match_rule(MatchRule::builder().msg_type(Signal).interface(SERVER)?.path(SERVER_PATH)?.build())?;
    dbus.add_match_rule(
        MatchRule::builder().msg_type(Signal).sender("org.freedesktop.DBus")?.member("NameOwnerChanged")?.arg(0, SERVER)?.build(),
    )?;
    // After subscribing: a server change from now on is seen in `signals`.
    let owner = match dbus.get_name_owner(SERVER.try_into()?) {
        Ok(o) => o.to_string(),
        Err(zbus::fdo::Error::NameHasNoOwner(_)) => String::new(),
        Err(e) => return Err(e.into()),
    };
    lock(shown).resume(&owner);
    for msg in signals {
        let open = lock(shown).on_signal(&msg?);
        if let Some(id) = open {
            clicked(hub, id);
        }
    }
    Ok(())
}

/// (title, text, id to open) for each notification.
pub fn plan(new: &[Summary]) -> Vec<(String, String, Option<i64>)> {
    match new {
        [] => vec![],
        n if n.len() > EACH => vec![(i18n::new_messages(n.len()), String::new(), Some(n[0].id))],
        n => n.iter().map(|m| {
            let who = if m.from.name.is_empty() { &m.from.addr } else { &m.from.name };
            (who.clone(), m.subject.clone(), Some(m.id))
        }).collect(),
    }
}

/// Notifies about unread Inbox mail newer than `notified_uid`, then marks everything up to
/// `last_uid` as notified. Failures to notify are logged, never returned.
pub fn announce(db: &Db, folder: &Folder) {
    announce_with(db, folder, show);
}

fn announce_with(db: &Db, folder: &Folder, show: impl Fn(&str, &str, Option<i64>)) {
    let new = match db.unread_inbox(&folder.account) {
        Ok(n) => n,
        Err(e) => return eprintln!("{}: notify: {e}", folder.account),
    };
    // Marks first: a failing notification daemon must not make every sync retry.
    if let Err(e) = db.set_folder_marks(folder.id, folder.last_uid, folder.last_uid.max(folder.notified_uid)) {
        return eprintln!("{}: notify: {e}", folder.account);
    }
    for (title, text, id) in plan(&new) {
        show(&title, &text, id);
    }
}

fn show(title: &str, text: &str, id: Option<i64>) {
    let shown = Notification::new()
        .appname(if i18n::is_ru() { "Почта" } else { "Mail" })
        .summary(title)
        .body(text)
        .icon("mail-unread")
        .hint(Hint::DesktopEntry(DESKTOP_ID.into()))
        .hint(Hint::Category("email.arrived".into()))
        .action("default", "default")
        .show();
    match shown {
        // The listener needs the server's broadcast signals, not this handle's connection.
        Ok(handle) => if let Some(id) = id { lock(&SHOWN).record(handle.id(), id) },
        Err(e) => eprintln!("notify: {e}"),
    }
}

fn clicked(hub: &Hub, id: i64) {
    if hub.has_window() {
        return hub.emit(Event::Open { id });
    }
    let uwsm = std::env::var_os("PATH").is_some_and(|p| std::env::split_paths(&p).any(|d| d.join("uwsm-app").is_file()));
    let argv = open_command(id, uwsm);
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    // SAFETY: setsid is async-signal-safe and touches no memory.
    unsafe { cmd.pre_exec(|| if libc::setsid() < 0 { Err(std::io::Error::last_os_error()) } else { Ok(()) }) };
    match cmd.spawn() {
        // Reaped, or it stays defunct; may wait long when `uwsm-app` lingers.
        Ok(mut child) => drop(std::thread::spawn(move || drop(child.wait()))),
        Err(e) => eprintln!("notify: {}: {e}", argv.join(" ")),
    }
}

pub fn open_command(id: i64, uwsm_in_path: bool) -> Vec<String> {
    let mut v: Vec<String> = if uwsm_in_path { vec!["uwsm-app".into(), "--".into()] } else { vec![] };
    v.extend(["omarchy-mail".into(), "--open".into(), id.to_string()]);
    v
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::Role;
    use crate::mime::{Addr, Parsed};

    fn msg(db: &Db, f: i64, uid: u32, seen: bool, name: &str) {
        let p = Parsed { subject: format!("s{uid}"), from: Addr { name: name.into(), addr: "a@y".into() }, date: uid as i64, ..Default::default() };
        db.insert_message(f, uid, seen, false, 1, &p, true).unwrap();
    }

    fn summaries(n: u32, name: &str) -> Vec<Summary> {
        let db = Db::memory();
        let f = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        (1..=n).for_each(|u| msg(&db, f, u, false, name));
        db.unread_inbox("me@x").unwrap()
    }

    #[test]
    fn plan_single() {
        let s = summaries(1, "Ann");
        assert_eq!(plan(&s), [("Ann".into(), "s1".into(), Some(s[0].id))]);
        assert_eq!(plan(&summaries(1, ""))[0].0, "a@y");
        assert!(plan(&[]).is_empty());
    }

    #[test]
    fn plan_batch() {
        let s = summaries(5, "Ann");
        let p = plan(&s);
        assert_eq!(p.len(), 1);
        assert_eq!(p[0].0, i18n::new_messages(5));
        assert_eq!(p[0].2, Some(s[0].id));
        assert_eq!(plan(&summaries(3, "Ann")).len(), 3);
    }

    #[test]
    fn no_notify_on_first_sync() {
        // what the first sync leaves: everything present counts as notified
        let db = Db::memory();
        let f = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        (1..=3).for_each(|u| msg(&db, f, u, false, "A"));
        db.set_folder_marks(f, 3, 3).unwrap();
        assert!(db.unread_inbox("me@x").unwrap().is_empty());
    }

    #[test]
    fn no_renotify_after_restart() {
        let path = std::env::temp_dir().join(format!("maild-test-{}-notify.db", std::process::id()));
        let _ = std::fs::remove_file(&path);
        {
            let db = Db::open(&path).unwrap();
            let f = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
            msg(&db, f, 1, false, "A");
            db.set_folder_marks(f, 1, 1).unwrap();
            msg(&db, f, 2, false, "A");
            db.set_folder_marks(f, 2, 1).unwrap();
            assert_eq!(db.unread_inbox("me@x").unwrap().len(), 1);
        }
        let db = Db::open(&path).unwrap();
        let f = db.folders_of("me@x").unwrap().remove(0);
        assert_eq!(f.notified_uid, 1);
        // announce marks the folder; with nothing shown (no mail) nothing pops up in tests
        db.set_folder_marks(f.id, 2, 2).unwrap();
        assert!(Db::open(&path).unwrap().unread_inbox("me@x").unwrap().is_empty());
        let _ = std::fs::remove_file(&path);
    }

    fn signal(path: &str, iface: &str, member: &str, body: &(impl serde::Serialize + zbus::zvariant::DynamicType)) -> zbus::Message {
        zbus::Message::signal(path, iface, member).unwrap().build(body).unwrap()
    }

    fn action(id: u32, key: &str) -> zbus::Message {
        signal(SERVER_PATH, SERVER, "ActionInvoked", &(id, key))
    }

    fn closed(id: u32) -> zbus::Message {
        signal(SERVER_PATH, SERVER, "NotificationClosed", &(id, 2u32))
    }

    #[test]
    fn click_opens_the_recorded_message() {
        let mut s = Shown::default();
        s.record(7, 101);
        s.record(8, 102);
        assert_eq!(s.on_signal(&action(7, "default")), Some(101));
        assert_eq!(s.on_signal(&action(8, "default")), Some(102));
        // clicked twice (toast, then the center): opens again
        assert_eq!(s.on_signal(&action(7, "default")), Some(101));
        assert_eq!(s.on_signal(&action(7, "reply")), None);
        assert_eq!(s.on_signal(&action(9, "default")), None, "another app's or a previous run's id");
        assert_eq!(s.on_signal(&signal("/other", SERVER, "ActionInvoked", &(7u32, "default"))), None);
        assert_eq!(s.on_signal(&signal(SERVER_PATH, SERVER, "ActionInvoked", &(7u32,))), None, "bad body");
    }

    #[test]
    fn close_forgets() {
        let mut s = Shown::default();
        s.record(7, 101);
        s.record(8, 102);
        assert_eq!(s.on_signal(&closed(7)), None);
        assert_eq!(s.on_signal(&action(7, "default")), None);
        assert_eq!(s.on_signal(&action(8, "default")), Some(102));
        s.on_signal(&closed(42));
        assert_eq!(s.ids.len(), 1);
    }

    #[test]
    fn server_restart_forgets_all() {
        let mut s = Shown::default();
        s.record(7, 101);
        let owner = |name: &str| signal("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged", &(name, ":1.5", ""));
        s.on_signal(&owner("org.example.Other"));
        assert_eq!(s.ids.len(), 1);
        s.on_signal(&owner(SERVER));
        assert_eq!(s.on_signal(&action(7, "default")), None);
    }

    fn tmp_file(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("maild-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        d.join("omarchy-mail/notify-ids.json")
    }

    fn with_file(f: &std::path::Path) -> Shown {
        Shown { file: Some(f.into()), ..Default::default() }
    }

    fn saved(f: &std::path::Path) -> (String, Vec<(u32, i64)>) {
        let s: Shown = serde_json::from_slice(&std::fs::read(f).unwrap()).unwrap();
        (s.owner, s.ids.into())
    }

    fn mode(p: &std::path::Path) -> u32 {
        use std::os::unix::fs::PermissionsExt;
        std::fs::metadata(p).unwrap().permissions().mode() & 0o777
    }

    #[test]
    fn saved_ids_survive_a_restart() {
        let f = tmp_file("notify-restart");
        let mut s = with_file(&f);
        s.resume(":1.5");
        s.record(7, 101);
        s.record(8, 102);
        s.closed(7);
        assert_eq!(saved(&f), (":1.5".into(), vec![(8, 102)]));
        assert_eq!((mode(f.parent().unwrap()), mode(&f)), (0o700, 0o600));
        // the next maild, the same server
        let mut s = with_file(&f);
        s.record(9, 103); // shown before the listener got going
        s.resume(":1.5");
        assert_eq!(s.clicked(8, "default"), Some(102));
        assert_eq!(s.clicked(9, "default"), Some(103));
        assert_eq!(s.clicked(7, "default"), None);
        // reconnecting to the same server keeps what is in memory
        s.file = None;
        s.resume(":1.5");
        assert_eq!(s.ids.len(), 2);
        let _ = std::fs::remove_dir_all(f.parent().unwrap().parent().unwrap());
    }

    #[test]
    fn saved_ids_of_another_server_are_dropped() {
        let f = tmp_file("notify-owner");
        let mut s = with_file(&f);
        s.resume(":1.5");
        s.record(8, 102);
        // a restarted server, seen by the next maild
        let mut s = with_file(&f);
        s.resume(":1.6");
        assert_eq!(s.clicked(8, "default"), None);
        assert_eq!(saved(&f), (":1.6".into(), vec![]));
        // seen by the running maild: NameOwnerChanged
        s.record(1, 201);
        s.owner_changed(":1.7");
        assert_eq!(s.clicked(1, "default"), None);
        assert_eq!(saved(&f), (":1.7".into(), vec![]));
        // a reconnect finding another server forgets what is in memory
        s.record(2, 202);
        s.resume(":1.8");
        assert!(s.ids.is_empty());
        let _ = std::fs::remove_dir_all(f.parent().unwrap().parent().unwrap());
    }

    #[test]
    fn corrupt_or_missing_file_is_ignored() {
        let f = tmp_file("notify-corrupt");
        let mut s = with_file(&f);
        s.resume(":1.5");
        assert!(s.ids.is_empty());
        std::fs::write(&f, "{not json").unwrap();
        let mut s = with_file(&f);
        s.resume(":1.5");
        assert!(s.ids.is_empty());
        assert_eq!(saved(&f), (":1.5".into(), vec![]));
        // too many: the newest KEEP
        let many: VecDeque<(u32, i64)> = (0..KEEP as u32 + 3).map(|n| (n, n as i64)).collect();
        std::fs::write(&f, serde_json::to_vec(&Shown { owner: ":1.5".into(), ids: many, ..Default::default() }).unwrap()).unwrap();
        let mut s = with_file(&f);
        s.resume(":1.5");
        assert_eq!((s.ids.len(), s.ids[0].0), (KEEP, 3));
        let _ = std::fs::remove_dir_all(f.parent().unwrap().parent().unwrap());
    }

    /// The real listener on a private dbus-daemon, a fake server emitting the signals.
    #[test]
    fn listener_on_a_private_bus() {
        use std::io::BufRead;
        use std::sync::Arc;
        use std::time::Instant;
        let dir = std::env::temp_dir().join(format!("maild-test-{}-bus", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        /// Killed however the test ends: a leftover daemon would keep `cargo test`'s output open.
        struct Daemon(std::process::Child, PathBuf);
        impl Drop for Daemon {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
                let _ = std::fs::remove_dir_all(&self.1);
            }
        }
        let Ok(child) = Command::new("dbus-daemon")
            .args(["--session", "--nofork", "--print-address"])
            .arg(format!("--address=unix:dir={}", dir.display()))
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
        else {
            return eprintln!("no dbus-daemon: skipped");
        };
        let mut daemon = Daemon(child, dir);
        let mut addr = String::new();
        std::io::BufReader::new(daemon.0.stdout.take().unwrap()).read_line(&mut addr).unwrap();
        let file = daemon.1.join("rt/omarchy-mail/notify-ids.json");
        let connect = || zbus::blocking::connection::Builder::address(addr.trim()).unwrap().build().unwrap();
        let hub = Hub::default();
        // A window: a click becomes an `open` event, nothing is launched.
        let rx = hub.subscribe(true);
        let next_open = |wait: Duration| {
            let end = Instant::now() + wait;
            while let Some(left) = end.checked_duration_since(Instant::now()) {
                match rx.recv_timeout(left) {
                    Ok(line) if line.is_empty() => continue,
                    Ok(line) => return Some(line),
                    Err(_) => break,
                }
            }
            None
        };
        let server = connect();
        server.request_name(SERVER).unwrap();
        let owner = server.unique_name().unwrap().to_string();
        let emit = |member: &str, body: &(u32, &str)| {
            server.emit_signal(None::<&str>, SERVER_PATH, SERVER, member, body).unwrap()
        };
        // One maild run: its own map, the same file.
        let run = || {
            let shown = Arc::new(Mutex::new(with_file(&file)));
            let (conn, h, s) = (connect(), hub.clone(), shown.clone());
            let keep = conn.clone();
            (shown, keep, std::thread::spawn(move || listen_on(conn, &h, &s)))
        };
        let open = |id: i64| format!(r#"{{"event":"open","id":{id}}}"#);
        let (a, conn_a, thread_a) = run();
        // Until the listener has subscribed.
        let start = Instant::now();
        loop {
            lock(&a).record(7, 101);
            emit("ActionInvoked", &(7, "default"));
            if let Some(e) = next_open(Duration::from_millis(100)) {
                break assert_eq!(e, open(101));
            }
            assert!(start.elapsed() < Duration::from_secs(10), "listener never heard the click");
        }
        // A slow round above may have heard two clicks.
        while next_open(Duration::from_millis(200)).is_some() {}
        lock(&a).record(8, 102);
        server.emit_signal(None::<&str>, SERVER_PATH, SERVER, "NotificationClosed", &(7u32, 2u32)).unwrap();
        emit("ActionInvoked", &(7, "default"));
        emit("ActionInvoked", &(8, "reply"));
        emit("ActionInvoked", &(99, "default"));
        emit("ActionInvoked", &(8, "default"));
        assert_eq!(next_open(Duration::from_secs(5)), Some(open(102)));
        assert_eq!(next_open(Duration::from_millis(200)), None);
        assert_eq!(saved(&file), (owner.clone(), vec![(8, 102)]));
        // maild restarts: a new, empty map; the notification left in the center still opens.
        conn_a.close().unwrap();
        let start = Instant::now();
        while !thread_a.is_finished() {
            assert!(start.elapsed() < Duration::from_secs(5), "listener kept running");
            std::thread::sleep(Duration::from_millis(10));
        }
        let (b, _conn_b, _) = run();
        let start = Instant::now();
        loop {
            emit("ActionInvoked", &(8, "default"));
            if let Some(e) = next_open(Duration::from_millis(100)) {
                break assert_eq!(e, open(102));
            }
            assert!(start.elapsed() < Duration::from_secs(10), "restarted listener never opened the old notification");
        }
        // The server goes away: its ids are forgotten, also in the file.
        server.release_name(SERVER).unwrap();
        let start = Instant::now();
        while !lock(&b).ids.is_empty() {
            assert!(start.elapsed() < Duration::from_secs(5), "server restart not seen");
            std::thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(saved(&file), (String::new(), vec![]));
    }

    #[test]
    fn keeps_the_newest() {
        let mut s = Shown::default();
        (0..KEEP as u32 + 5).for_each(|n| s.record(n, n as i64 + 100));
        assert_eq!(s.ids.len(), KEEP);
        assert_eq!(s.clicked(4, "default"), None);
        assert_eq!(s.clicked(5, "default"), Some(105));
        assert_eq!(s.clicked(KEEP as u32 + 4, "default"), Some(KEEP as i64 + 104));
    }

    fn folder(db: &Db) -> Folder {
        db.folders_of("me@x").unwrap().remove(0)
    }

    #[test]
    fn announce_marks_and_does_not_repeat() {
        use std::cell::RefCell;
        let db = Db::memory();
        let f = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        db.set_folder_marks(f, 0, 0).unwrap();
        (1..=5).for_each(|u| msg(&db, f, u, false, "A"));
        db.set_folder_marks(f, 5, 0).unwrap();
        let shown = RefCell::new(vec![]);
        let rec = |t: &str, b: &str, id| shown.borrow_mut().push((t.to_string(), b.to_string(), id));
        announce_with(&db, &folder(&db), rec);
        assert_eq!(shown.borrow().len(), 1);
        assert_eq!(shown.borrow()[0].0, i18n::new_messages(5));
        let fo = folder(&db);
        assert_eq!((fo.last_uid, fo.notified_uid), (5, 5));
        announce_with(&db, &fo, rec);
        assert_eq!(shown.borrow().len(), 1);
    }

    #[test]
    fn open_command_prefix() {
        assert_eq!(open_command(7, false), ["omarchy-mail", "--open", "7"]);
        assert_eq!(open_command(7, true), ["uwsm-app", "--", "omarchy-mail", "--open", "7"]);
    }
}
