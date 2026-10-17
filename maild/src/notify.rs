//! New-mail notifications: what to show (`plan`), showing it, and what a click does.

use crate::db::{Db, Folder, Summary};
use crate::hub::{Event, Hub, lock};
use crate::i18n;
use notify_rust::{Hint, Notification};
use std::collections::VecDeque;
use std::os::unix::process::CommandExt;
use std::process::{Command, Stdio};
use std::sync::{Mutex, Once};
use std::time::Duration;

/// Up to this many new messages get a notification each; more are folded into one.
const EACH: usize = 3;
const DESKTOP_ID: &str = "omarchy-mail";
/// The notification server's bus name, also its interface.
const SERVER: &str = "org.freedesktop.Notifications";
const SERVER_PATH: &str = "/org/freedesktop/Notifications";
/// Clickable notifications remembered; older ones (still in the center) open nothing.
const KEEP: usize = 1000;

static SHOWN: Mutex<Shown> = Mutex::new(Shown(VecDeque::new()));
static LISTENER: Once = Once::new();

/// (server notification id, message id) of the shown notifications, oldest first.
struct Shown(VecDeque<(u32, i64)>);

impl Shown {
    fn record(&mut self, id: u32, msg: i64) {
        if self.0.len() >= KEEP {
            self.0.pop_front();
        }
        self.0.push_back((id, msg));
    }

    /// The message to open for `ActionInvoked(id, key)`.
    fn clicked(&self, id: u32, key: &str) -> Option<i64> {
        if key != "default" {
            return None;
        }
        self.0.iter().rev().find(|e| e.0 == id).map(|e| e.1)
    }

    fn closed(&mut self, id: u32) {
        self.0.retain(|e| e.0 != id);
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
            // A restarted server numbers from 1 again: our ids now belong to other apps.
            ("org.freedesktop.DBus", "NameOwnerChanged") => {
                let (name, _, _): (String, String, String) = msg.body().deserialize().ok()?;
                if name == SERVER {
                    self.0.clear();
                }
                None
            }
            _ => None,
        }
    }
}

/// Runs for the service's lifetime: one session bus connection for every notification's clicks.
fn listen(hub: Hub) {
    loop {
        if let Err(e) = zbus::blocking::Connection::session().and_then(|c| listen_on(c, &hub)) {
            eprintln!("notify: click listener: {e}");
        }
        // Off the bus the server may have restarted unseen.
        lock(&SHOWN).0.clear();
        std::thread::sleep(Duration::from_secs(5));
    }
}

/// Returns when the connection drops.
fn listen_on(conn: zbus::blocking::Connection, hub: &Hub) -> zbus::Result<()> {
    use zbus::{MatchRule, message::Type::Signal};
    let signals = zbus::blocking::MessageIterator::from(conn.clone());
    let dbus = zbus::blocking::fdo::DBusProxy::new(&conn)?;
    dbus.add_match_rule(MatchRule::builder().msg_type(Signal).interface(SERVER)?.path(SERVER_PATH)?.build())?;
    dbus.add_match_rule(
        MatchRule::builder().msg_type(Signal).sender("org.freedesktop.DBus")?.member("NameOwnerChanged")?.arg(0, SERVER)?.build(),
    )?;
    for msg in signals {
        let open = lock(&SHOWN).on_signal(&msg?);
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
pub fn announce(db: &Db, hub: &Hub, folder: &Folder) {
    announce_with(db, folder, |t, b, id| show(hub, t, b, id));
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

fn show(hub: &Hub, title: &str, text: &str, id: Option<i64>) {
    // Subscribing takes milliseconds, well before anyone clicks the first notification.
    LISTENER.call_once(|| {
        let hub = hub.clone();
        std::thread::spawn(move || listen(hub));
    });
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
        let mut s = Shown(VecDeque::new());
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
        let mut s = Shown(VecDeque::new());
        s.record(7, 101);
        s.record(8, 102);
        assert_eq!(s.on_signal(&closed(7)), None);
        assert_eq!(s.on_signal(&action(7, "default")), None);
        assert_eq!(s.on_signal(&action(8, "default")), Some(102));
        s.on_signal(&closed(42));
        assert_eq!(s.0.len(), 1);
    }

    #[test]
    fn server_restart_forgets_all() {
        let mut s = Shown(VecDeque::new());
        s.record(7, 101);
        let owner = |name: &str| signal("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged", &(name, ":1.5", ""));
        s.on_signal(&owner("org.example.Other"));
        assert_eq!(s.0.len(), 1);
        s.on_signal(&owner(SERVER));
        assert_eq!(s.on_signal(&action(7, "default")), None);
    }

    /// The real listener on a private dbus-daemon, a fake server emitting the signals.
    #[test]
    fn listener_on_a_private_bus() {
        use std::io::BufRead;
        use std::time::Instant;
        let dir = std::env::temp_dir().join(format!("maild-test-{}-bus", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        /// Killed however the test ends: a leftover daemon would keep `cargo test`'s output open.
        struct Daemon(std::process::Child, std::path::PathBuf);
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
        let listener = connect();
        let h = hub.clone();
        std::thread::spawn(move || listen_on(listener, &h));
        let server = connect();
        server.request_name(SERVER).unwrap();
        let emit = |member: &str, body: &(u32, &str)| {
            server.emit_signal(None::<&str>, SERVER_PATH, SERVER, member, body).unwrap()
        };
        // Until the listener has subscribed (and seen the name taken, which forgets everything).
        let start = Instant::now();
        loop {
            lock(&SHOWN).record(7, 101);
            emit("ActionInvoked", &(7, "default"));
            if let Some(e) = next_open(Duration::from_millis(100)) {
                break assert_eq!(e, r#"{"event":"open","id":101}"#);
            }
            assert!(start.elapsed() < Duration::from_secs(10), "listener never heard the click");
        }
        // A slow round above may have heard two clicks.
        while next_open(Duration::from_millis(200)).is_some() {}
        lock(&SHOWN).record(8, 102);
        server.emit_signal(None::<&str>, SERVER_PATH, SERVER, "NotificationClosed", &(7u32, 2u32)).unwrap();
        emit("ActionInvoked", &(7, "default"));
        emit("ActionInvoked", &(8, "reply"));
        emit("ActionInvoked", &(99, "default"));
        emit("ActionInvoked", &(8, "default"));
        assert_eq!(next_open(Duration::from_secs(5)).as_deref(), Some(r#"{"event":"open","id":102}"#));
        assert_eq!(next_open(Duration::from_millis(200)), None);
        // The server goes away: its ids are forgotten.
        server.release_name(SERVER).unwrap();
        let start = Instant::now();
        while !lock(&SHOWN).0.is_empty() {
            assert!(start.elapsed() < Duration::from_secs(5), "server restart not seen");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    #[test]
    fn keeps_the_newest() {
        let mut s = Shown(VecDeque::new());
        (0..KEEP as u32 + 5).for_each(|n| s.record(n, n as i64 + 100));
        assert_eq!(s.0.len(), KEEP);
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
