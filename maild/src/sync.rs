//! Incremental folder sync. Read-only on the server: EXAMINE and BODY.PEEK, never STORE/EXPUNGE.

use crate::config::Account;
use crate::db::{Db, Folder, Role};
use crate::imap::{Conn, text_sections};
use crate::mime::{self, Parsed};
use ::imap::types::{Fetch, Flag};
use imap_proto::SectionPath;
use std::collections::HashMap;

/// Local history starts here (IMAP `SINCE`).
pub const SINCE: &str = "1-Jan-2026";
/// Bigger messages get headers + text parts only; the rest is fetched on demand.
pub const FULL_FETCH_LIMIT: u32 = 2 * 1024 * 1024;
const BATCH: usize = 50;

/// UIDVALIDITY changed: the stored UIDs mean nothing any more.
pub fn needs_reset(stored: u32, server: u32) -> bool {
    stored != 0 && stored != server
}

fn uid_set(uids: &[u32]) -> String {
    uids.iter().map(u32::to_string).collect::<Vec<_>>().join(",")
}

fn flags(f: &Fetch) -> (bool, bool) {
    (f.flags().contains(&Flag::Seen), f.flags().contains(&Flag::Flagged))
}

/// Returns ids of newly inserted messages.
pub fn sync_folder(conn: &mut Conn, db: &Db, folder: &Folder) -> Result<Vec<i64>, String> {
    let mb = conn.examine(&folder.path)?;
    // A server that omits UIDVALIDITY keeps the stored one instead of forcing a re-import.
    let validity = mb.uid_validity.unwrap_or(folder.uidvalidity);
    let mut last = folder.last_uid;
    if needs_reset(folder.uidvalidity, validity) || (folder.uidvalidity == 0 && validity != 0) {
        db.reset_folder(folder.id, validity)?;
        last = 0;
    }
    let first = last == 0;
    let new: Vec<u32> = if first {
        conn.uids(&format!("SINCE {SINCE}"))?
    } else {
        // `n:*` always matches the highest UID, even when it is below n.
        conn.uids(&format!("UID {}:*", last + 1))?.into_iter().filter(|&u| u > last).collect()
    };
    // The initial import is silent: everything present now counts as notified.
    let notified = if first {
        new.last().copied().unwrap_or(0).max(mb.uid_next.unwrap_or(1).saturating_sub(1))
    } else {
        folder.notified_uid
    };

    let mut ids = vec![];
    for chunk in new.chunks(BATCH) {
        ids.extend(fetch_new(conn, db, folder.id, chunk)?);
        last = last.max(chunk[chunk.len() - 1]);
        // Saved per batch so a dropped connection does not restart a long import.
        db.set_folder_marks(folder.id, last, notified)?;
    }
    if first {
        last = last.max(notified);
    }

    // Flags and deletions, only over the UID range we hold locally.
    match db.min_uid(folder.id)? {
        None => {}
        Some(_) if mb.exists == 0 => db.remove_missing(folder.id, &[])?,
        Some(min) => {
            let mut present = vec![];
            for f in conn.fetch(&format!("{min}:*"), "(UID FLAGS)")?.iter() {
                let Some(uid) = f.uid.filter(|&u| u >= min) else { continue };
                let (seen, flagged) = flags(f);
                db.set_flags(folder.id, uid, seen, flagged)?;
                present.push(uid);
            }
            db.remove_missing(folder.id, &present)?;
        }
    }
    db.set_folder_marks(folder.id, last, notified)?;
    Ok(ids)
}

// ponytail: up to 50 × 2 MiB bodies held in memory per batch; cap the batch by total size if that bites.
fn fetch_new(conn: &mut Conn, db: &Db, folder: i64, uids: &[u32]) -> Result<Vec<i64>, String> {
    let meta = conn.fetch(&uid_set(uids), "(UID FLAGS RFC822.SIZE)")?;
    let info: Vec<(u32, bool, bool, u32)> = meta.iter().filter_map(|f| {
        let uid = f.uid.filter(|u| uids.contains(u))?;
        let (seen, flagged) = flags(f);
        Some((uid, seen, flagged, f.size.unwrap_or(0)))
    }).collect();
    let small: Vec<u32> = info.iter().filter(|i| i.3 <= FULL_FETCH_LIMIT).map(|i| i.0).collect();
    let big: Vec<u32> = info.iter().filter(|i| i.3 > FULL_FETCH_LIMIT).map(|i| i.0).collect();
    let bodies = if small.is_empty() { None } else { Some(conn.fetch(&uid_set(&small), "(UID BODY.PEEK[])")?) };
    let heads = if big.is_empty() { None } else { Some(conn.fetch(&uid_set(&big), "(UID BODY.PEEK[HEADER] BODYSTRUCTURE)")?) };
    let body_of: HashMap<u32, &[u8]> = bodies.iter().flat_map(|b| b.iter()).filter_map(|f| Some((f.uid?, f.body()?))).collect();
    let head_of: HashMap<u32, &Fetch> = heads.iter().flat_map(|h| h.iter()).filter_map(|f| Some((f.uid?, f))).collect();

    let mut ids = vec![];
    for &(uid, seen, flagged, size) in &info {
        // A UID missing from the answer was expunged meanwhile.
        let (p, has_body) = if size <= FULL_FETCH_LIMIT {
            let Some(raw) = body_of.get(&uid) else { continue };
            (mime::parse(raw), true)
        } else {
            let Some(head) = head_of.get(&uid) else { continue };
            big_message(conn, uid, head)?
        };
        ids.push(db.insert_message(folder, uid, seen, flagged, size as u64, &p, has_body)?);
    }
    Ok(ids)
}

/// Headers plus the first text/plain and text/html parts; `has_body` if any text part came.
fn big_message(conn: &mut Conn, uid: u32, head: &Fetch) -> Result<(Parsed, bool), String> {
    let mut p = mime::parse(head.header().unwrap_or_default());
    let secs = head.bodystructure().map(text_sections).unwrap_or_default();
    let plain = secs.iter().find(|s| s.1.starts_with("text/plain"));
    let html = secs.iter().find(|s| s.1.starts_with("text/html"));
    let query: Vec<String> = [plain, html].into_iter().flatten().map(|s| format!("BODY.PEEK[{}]", s.0)).collect();
    if query.is_empty() {
        return Ok((p, false));
    }
    let parts = conn.fetch(&uid.to_string(), &format!("(UID {})", query.join(" ")))?;
    let Some(f) = parts.iter().find(|f| f.uid == Some(uid)) else { return Ok((p, false)) };
    let get = |s: &(String, String, String)| {
        let path = SectionPath::Part(s.0.split('.').filter_map(|n| n.parse().ok()).collect(), None);
        f.section(&path).map(|b| mime::decode_part(b, &s.1, &s.2))
    };
    let mut has_body = false;
    if let Some(t) = plain.and_then(get) {
        p.text = t;
        has_body = true;
    }
    if let Some(h) = html.and_then(get) {
        p.html = Some(mime::sanitize_html(&h));
        has_body = true;
    }
    p.preview = mime::preview(&p.text, p.html.as_deref());
    Ok((p, has_body))
}

/// Refreshes the folder list, then syncs Inbox first and the rest after it.
pub fn sync_account(conn: &mut Conn, db: &Db, acc: &Account) -> Result<Vec<i64>, String> {
    let (found, _all) = conn.discover()?;
    if found.is_empty() {
        // Never wipe the local store on an odd LIST answer.
        return Err("server listed no folders".into());
    }
    for (path, role) in &found {
        db.upsert_folder(&acc.email, path, *role)?;
    }
    let mut folders = vec![];
    for f in db.folders_of(&acc.email)? {
        if f.role == Role::Remote {
            continue;
        }
        if found.iter().any(|(p, _)| *p == f.path) { folders.push(f) } else { db.remove_folder(f.id)? }
    }
    folders.sort_by_key(|f| f.role != Role::Inbox);
    let mut ids = vec![];
    for f in &folders {
        match sync_folder(conn, db, f) {
            Ok(v) => ids.extend(v),
            // One broken folder must not stall the account; a dead connection goes to the caller.
            Err(e) if conn.noop().is_ok() => eprintln!("{}: {}: {e}", acc.email, f.path),
            Err(e) => return Err(e),
        }
    }
    Ok(ids)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Provider, Server};
    use crate::db::ListQuery;
    use std::io::{BufRead, BufReader, Write};
    use std::net::{TcpListener, TcpStream};

    #[test]
    fn uidvalidity_change_resets_folder() {
        assert!(needs_reset(7, 8));
        assert!(!needs_reset(7, 7));
        assert!(!needs_reset(0, 8));
    }

    /// One-connection IMAP server: greets, then answers each command with `reply(cmd)` + tagged OK;
    /// an unexpected command gets BAD.
    fn fake(reply: fn(&str) -> Option<String>) -> Conn {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap();
        std::thread::spawn(move || {
            let (s, _) = l.accept().unwrap();
            let mut w = s.try_clone().unwrap();
            w.write_all(b"* OK fake\r\n").unwrap();
            for line in BufReader::new(s).lines() {
                let Ok(line) = line else { break };
                let (tag, cmd) = line.split_once(' ').unwrap();
                let out = match reply(cmd) {
                    Some(r) => format!("{r}{tag} OK done\r\n"),
                    None => format!("{tag} BAD unexpected {cmd}\r\n"),
                };
                if w.write_all(out.as_bytes()).is_err() { break }
            }
        });
        Conn::plain(TcpStream::connect(addr).unwrap(), "me", "pw").unwrap()
    }

    fn lit(s: &str) -> String { format!("{{{}}}\r\n{s}", s.len()) }

    const M1: &str = "Subject: one\r\nFrom: a@x.org\r\n\r\nhello one\r\n";
    const M2: &str = "Subject: two\r\nFrom: a@x.org\r\n\r\nhello two\r\n";

    fn before(cmd: &str) -> Option<String> {
        Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n* LIST (\\HasNoChildren \\All) \"/\" \"[Gmail]/All Mail\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 2 EXISTS\r\n* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 3] n\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 2 1\r\n".into(),
            "UID FETCH 1,2 (UID FLAGS RFC822.SIZE)" => "* 1 FETCH (UID 1 FLAGS (\\Seen) RFC822.SIZE 60)\r\n* 2 FETCH (UID 2 FLAGS () RFC822.SIZE 60)\r\n".into(),
            "UID FETCH 1,2 (UID BODY.PEEK[])" => format!("* 1 FETCH (UID 1 BODY[] {})\r\n* 2 FETCH (UID 2 BODY[] {})\r\n", lit(M1), lit(M2)),
            "UID FETCH 1:* (UID FLAGS)" => "* 1 FETCH (UID 1 FLAGS (\\Seen))\r\n* 2 FETCH (UID 2 FLAGS ())\r\n".into(),
            _ => return None,
        })
    }

    /// uid 1 got flagged, uid 2 was deleted, uid 3 is a new big message.
    fn after(cmd: &str) -> Option<String> {
        let bs = "((\"text\" \"plain\" (\"charset\" \"utf-8\") NIL NIL \"7bit\" 9 1)(\"text\" \"html\" (\"charset\" \"utf-8\") NIL NIL \"base64\" 40 1) \"alternative\")";
        Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "EXAMINE \"INBOX\"" => "* 2 EXISTS\r\n* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 4] n\r\n".into(),
            "UID SEARCH UID 3:*" | "UID SEARCH UID 4:*" => "* SEARCH 3\r\n".into(),
            "UID FETCH 3 (UID FLAGS RFC822.SIZE)" => "* 2 FETCH (UID 3 FLAGS () RFC822.SIZE 3000000)\r\n".into(),
            "UID FETCH 3 (UID BODY.PEEK[HEADER] BODYSTRUCTURE)" =>
                format!("* 2 FETCH (UID 3 BODY[HEADER] {} BODYSTRUCTURE {bs})\r\n", lit("Subject: big\r\nFrom: b@x.org\r\n\r\n")),
            "UID FETCH 3 (UID BODY.PEEK[1] BODY.PEEK[2])" =>
                format!("* 2 FETCH (UID 3 BODY[1] {} BODY[2] {})\r\n", lit("big plain"), lit("PGI+YmlnPC9iPjxzY3JpcHQ+eDwvc2NyaXB0Pg==")),
            "UID FETCH 1:* (UID FLAGS)" => "* 1 FETCH (UID 1 FLAGS (\\Seen \\Flagged))\r\n* 2 FETCH (UID 3 FLAGS ())\r\n".into(),
            _ => return None,
        })
    }

    #[test]
    fn sync_imports_then_updates() {
        let db = Db::memory();
        db.upsert_folder("me@x", "Old", Role::Other).unwrap();
        db.upsert_folder("me@x", "[Gmail]/Remote", Role::Remote).unwrap();
        let s = Server { host: "h".into(), port: 1 };
        let acc = Account { email: "me@x".into(), name: "N".into(), provider: Provider::Gmail, login: "me".into(), imap: s.clone(), smtp: s, append_sent: false };

        assert_eq!(sync_account(&mut fake(before), &db, &acc).unwrap().len(), 2);
        let inbox = || db.folders_of("me@x").unwrap().into_iter().find(|f| f.role == Role::Inbox).unwrap();
        let paths: Vec<_> = db.folders_of("me@x").unwrap().into_iter().map(|f| f.path).collect();
        assert_eq!(paths, ["[Gmail]/Remote", "INBOX"]);
        let f = inbox();
        assert_eq!((f.uidvalidity, f.last_uid, f.notified_uid), (7, 2, 2));
        let list = || db.list(&ListQuery { folder: Some(f.id), ..Default::default() }).unwrap();
        assert!(list().iter().any(|m| m.subject == "one" && m.seen && !m.flagged));

        let ids = sync_folder(&mut fake(after), &db, &f).unwrap();
        assert_eq!(ids.len(), 1);
        let big = db.message(ids[0]).unwrap().unwrap();
        assert_eq!((big.uid, big.summary.subject.as_str(), big.text.as_str(), big.has_body), (Some(3), "big", "big plain", true));
        assert_eq!(big.html.as_deref().map(|h| h.contains("<b>big</b>") && !h.contains("script")), Some(true));
        let mut subjects: Vec<_> = list().into_iter().map(|m| (m.subject, m.flagged)).collect();
        subjects.sort();
        assert_eq!(subjects, [("big".into(), false), ("one".into(), true)]);
        let f = inbox();
        assert_eq!((f.last_uid, f.notified_uid), (3, 2));

        assert!(sync_folder(&mut fake(after), &db, &f).unwrap().is_empty());
    }
}
