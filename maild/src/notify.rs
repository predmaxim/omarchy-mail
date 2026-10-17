//! New-mail notifications: what to show (`plan`), showing it, and what a click does.

use crate::db::{Db, Folder, Summary};
use crate::hub::{Event, Hub};
use crate::i18n;
use notify_rust::{Hint, Notification};
use std::os::unix::process::CommandExt;
use std::process::{Command, Stdio};

/// Up to this many new messages get a notification each; more are folded into one.
const EACH: usize = 3;
const DESKTOP_ID: &str = "omarchy-mail";

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
    let new = match db.unread_inbox(&folder.account) {
        Ok(n) => n,
        Err(e) => return eprintln!("{}: notify: {e}", folder.account),
    };
    // Marks first: a failing notification daemon must not make every sync retry.
    if let Err(e) = db.set_folder_marks(folder.id, folder.last_uid, folder.last_uid.max(folder.notified_uid)) {
        return eprintln!("{}: notify: {e}", folder.account);
    }
    for (title, text, id) in plan(&new) {
        show(hub, &title, &text, id);
    }
}

fn show(hub: &Hub, title: &str, text: &str, id: Option<i64>) {
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
        // One short-lived thread per notification, until it is clicked or closed.
        Ok(handle) => {
            let hub = hub.clone();
            std::thread::spawn(move || handle.wait_for_action(|a| if a == "default" && let Some(id) = id { clicked(&hub, id) }));
        }
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
    if let Err(e) = cmd.spawn() {
        eprintln!("notify: {}: {e}", argv.join(" "));
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

    #[test]
    fn open_command_prefix() {
        assert_eq!(open_command(7, false), ["omarchy-mail", "--open", "7"]);
        assert_eq!(open_command(7, true), ["uwsm-app", "--", "omarchy-mail", "--open", "7"]);
    }
}
