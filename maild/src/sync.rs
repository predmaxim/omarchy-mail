//! Incremental folder sync. Read-only on the server: EXAMINE and BODY.PEEK, never STORE/EXPUNGE.

use crate::config::Account;
use crate::db::{Db, Folder, Role};
use crate::imap::{Conn, REFUSED, UNPARSEABLE, attachments, text_sections};
use crate::mime::{self, Parsed};
use ::imap::types::{Fetch, Flag};
use imap_proto::SectionPath;
use std::collections::{HashMap, HashSet};

/// Local history starts here (IMAP `SINCE`).
pub const SINCE: &str = "1-Jan-2026";
/// Bigger messages get headers + text parts only; the rest is fetched on demand.
pub const FULL_FETCH_LIMIT: u32 = 2 * 1024 * 1024;
const BATCH: usize = 50;
/// Unparseable messages one `sync_folder` call may pass over; each one costs a fresh login.
const MAX_SKIPS: usize = 5;
/// Messages the server refuses one `sync_folder` call may pass over (no login cost, only a sanity bound).
const MAX_REFUSED: usize = 200;
/// Failed retries after which a refused message is given up.
const MAX_TRIES: u32 = 20;
/// Refusals in a row with no success between that point to the whole command, not one message.
const SYSTEMIC: usize = 3;
/// Refused messages retried per folder and sync.
const RETRIES_PER_PASS: usize = 50;

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
        conn.uids(&format!("UID {}:*", last.saturating_add(1)))?.into_iter().filter(|&u| u > last).collect()
    };
    // The initial import is silent: everything present now counts as notified.
    let notified = if first {
        new.last().copied().unwrap_or(0).max(mb.uid_next.unwrap_or(1).saturating_sub(1))
    } else {
        folder.notified_uid
    };

    let mut ids = vec![];
    let (mut skips, mut refused) = (0, 0);
    for chunk in new.chunks(BATCH) {
        let r = match fetch_new(conn, chunk) {
            Ok(got) => store(db, folder.id, got, &mut ids).map(|_| last = last.max(chunk[chunk.len() - 1])),
            Err(e) if e.starts_with(UNPARSEABLE) => one_by_one(conn, db, folder, chunk, &mut ids, &mut last, &mut skips, &mut refused, true),
            // A tagged NO leaves the session in step: no new login.
            Err(e) if e.starts_with(REFUSED) => one_by_one(conn, db, folder, chunk, &mut ids, &mut last, &mut skips, &mut refused, false),
            Err(e) => Err(e),
        };
        // Saved per batch, and before giving up, so a dropped connection does not restart a long import.
        db.set_folder_marks(folder.id, last, notified)?;
        r?;
    }
    if first {
        last = last.max(notified);
    }

    // Flags and deletions, only over the UID range we hold locally. Presence comes from this answer
    // alone (not EXISTS, which a server may omit).
    match db.min_uid(folder.id)? {
        None => {}
        Some(min) => {
            let mut present = vec![];
            match conn.fetch(&format!("{min}:*"), "(UID FLAGS)") {
                Ok(got) => {
                    for f in got.iter() {
                        let Some(uid) = f.uid.filter(|&u| u >= min) else { continue };
                        let (seen, flagged) = flags(f);
                        db.set_flags(folder.id, uid, seen, flagged)?;
                        present.push(uid);
                    }
                    db.remove_missing(folder.id, &present)?;
                }
                // Without the answer nothing is known: keep everything, try again next time.
                Err(e) if e.starts_with(REFUSED) => eprintln!("{} {}: flags skipped: {e}", folder.account, folder.path),
                Err(e) => return Err(e),
            }
        }
    }
    db.set_folder_marks(folder.id, last, notified)?;
    Ok(ids)
}

fn store(db: &Db, folder: i64, got: Vec<New>, ids: &mut Vec<i64>) -> Result<(), String> {
    for m in got {
        ids.push(db.insert_message(folder, m.uid, m.seen, m.flagged, m.size.into(), &m.parsed, m.has_body)?);
    }
    Ok(())
}

/// The batch could not be fetched: fetch its UIDs one by one, passing over those that still fail
/// (for good, so one odd message cannot stall the folder). An unparseable answer leaves the session
/// out of step (`reopen_first`; a fresh login), a tagged NO does not. Unparseable ones past `MAX_SKIPS`
/// per sync give up with `last` at the last UID handled, so a broken server cannot cause a login storm.
/// Refused ones go to the `refused` table to be retried; past `MAX_REFUSED` the folder stops.
#[allow(clippy::too_many_arguments)]
fn one_by_one(conn: &mut Conn, db: &Db, folder: &Folder, uids: &[u32], ids: &mut Vec<i64>, last: &mut u32, skips: &mut usize, refused: &mut usize, reopen_first: bool) -> Result<(), String> {
    let reopen = |conn: &mut Conn| conn.reconnect().and_then(|_| conn.examine(&folder.path)).map(drop);
    if reopen_first {
        reopen(conn)?;
    }
    // Refusals are recorded once a later UID succeeds: three in a row from the start look like a
    // command-wide NO or an outage, and the batch is left for the next sync.
    let mut pending: Vec<u32> = vec![];
    let mut any_ok = false;
    for &uid in uids {
        match fetch_new(conn, &[uid]) {
            Ok(got) => {
                store(db, folder.id, got, ids)?;
                any_ok = true;
                for u in pending.drain(..) { db.add_refused(folder.id, u)?; }
            }
            Err(e) if e.starts_with(REFUSED) => {
                *refused += 1;
                if *refused > MAX_REFUSED {
                    return Err(format!("{e} (gave up after {MAX_REFUSED} refused messages)"));
                }
                if !any_ok && pending.len() + 1 >= SYSTEMIC {
                    return Err(e);
                }
                eprintln!("{} {}: uid {uid} refused by server: {e}", folder.account, folder.path);
                if any_ok { db.add_refused(folder.id, uid)? } else { pending.push(uid) }
            }
            Err(e) if e.starts_with(UNPARSEABLE) => {
                *skips += 1;
                if *skips > MAX_SKIPS {
                    return Err(format!("{e} (gave up after {MAX_SKIPS} skipped messages)"));
                }
                eprintln!("{} {}: uid {uid} skipped: {e}", folder.account, folder.path);
                reopen(conn)?;
            }
            Err(e) => return Err(e),
        }
        if pending.is_empty() { *last = (*last).max(uid); }
    }
    for u in pending { db.add_refused(folder.id, u)?; }
    if let Some(&u) = uids.last() { *last = (*last).max(u); }
    Ok(())
}

struct New { uid: u32, seen: bool, flagged: bool, size: u32, parsed: Parsed, has_body: bool }

// ponytail: up to 50 × 2 MiB bodies held in memory per batch; cap the batch by total size if that bites.
fn fetch_new(conn: &mut Conn, uids: &[u32]) -> Result<Vec<New>, String> {
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

    let mut out = vec![];
    for (uid, seen, flagged, size) in info {
        // A UID missing from the answer was expunged meanwhile.
        let (parsed, has_body) = if size <= FULL_FETCH_LIMIT {
            let Some(raw) = body_of.get(&uid) else { continue };
            (mime::parse(raw), true)
        } else {
            let Some(head) = head_of.get(&uid) else { continue };
            big_message(conn, uid, head)?
        };
        out.push(New { uid, seen, flagged, size, parsed, has_body });
    }
    Ok(out)
}

/// Headers, attachment list and the first text/plain and text/html parts; `has_body` if any text part came.
fn big_message(conn: &mut Conn, uid: u32, head: &Fetch) -> Result<(Parsed, bool), String> {
    let mut p = mime::parse(head.header().unwrap_or_default());
    let Some(bs) = head.bodystructure() else { return Ok((p, false)) };
    let secs = text_sections(bs);
    let plain = secs.iter().find(|s| s.1.starts_with("text/plain"));
    let html = secs.iter().find(|s| s.1.starts_with("text/html"));
    let body: Vec<&str> = [plain, html].into_iter().flatten().map(|s| s.0.as_str()).collect();
    p.attachments = attachments(bs, &body);
    let query: Vec<String> = body.iter().map(|s| format!("BODY.PEEK[{s}]")).collect();
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

/// Fetches again the messages of `old` the server refused earlier and that are still in the `refused`
/// table, in the folder just synced (`sync_folder` examined it); at most `RETRIES_PER_PASS`. Stored ones
/// leave the table, as do those gone from the server and those that failed `MAX_TRIES` times. Three
/// refusals in a row end the pass; if none succeeded before, it counts no failed try (an outage).
fn retry_refused(conn: &mut Conn, db: &Db, folder: &Folder, old: &[u32], ids: &mut Vec<i64>) -> Result<(), String> {
    let old: HashSet<u32> = old.iter().copied().collect();
    let (mut failed, mut run, mut any_ok) = (vec![], 0, false);
    for uid in db.refused_uids(folder.id)?.into_iter().filter(|u| old.contains(u)).take(RETRIES_PER_PASS) {
        match fetch_new(conn, &[uid]) {
            Ok(got) => {
                store(db, folder.id, got, ids)?;
                db.drop_refused(folder.id, uid)?;
                (run, any_ok) = (0, true);
            }
            Err(e) if e.starts_with(REFUSED) => {
                failed.push((uid, e));
                run += 1;
                if run >= SYSTEMIC { break }
            }
            Err(e) => return Err(e),
        }
    }
    if run >= SYSTEMIC && !any_ok {
        eprintln!("{} {}: retrying refused messages stopped: {}", folder.account, folder.path, failed[0].1);
        return Ok(());
    }
    for (uid, e) in failed {
        if db.bump_refused(folder.id, uid)? >= MAX_TRIES {
            eprintln!("{} {}: uid {uid} given up after {MAX_TRIES} tries: {e}", folder.account, folder.path);
            db.drop_refused(folder.id, uid)?;
        }
    }
    Ok(())
}

/// Refreshes the folder list, then syncs Inbox first and the rest after it. `shown` gets the ids of
/// each folder's new messages as soon as that folder is done (nothing for a folder without any).
pub fn sync_account(conn: &mut Conn, db: &Db, acc: &Account, mut shown: impl FnMut(Vec<i64>)) -> Result<(), String> {
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
    for f in &folders {
        let old = db.refused_uids(f.id)?;
        let mut v = vec![];
        let r = sync_folder(conn, db, f).and_then(|n| {
            v = n;
            if old.is_empty() { Ok(()) } else { retry_refused(conn, db, f, &old, &mut v) }
        });
        // New mail is shown even if retrying the refused ones failed.
        if !v.is_empty() { shown(v) }
        match r {
            Ok(()) => {}
            // One broken folder must not stall the account; a dead connection goes to the caller.
            Err(e) => {
                let alive = if e.starts_with(UNPARSEABLE) { conn.reconnect().is_ok() } else { conn.noop().is_ok() };
                if !alive { return Err(e); }
                eprintln!("{}: {}: {e}", acc.email, f.path);
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Provider, Server};
    use crate::db::ListQuery;
    use std::net::TcpStream;
    use std::sync::atomic::{AtomicUsize, Ordering};

    #[test]
    fn uidvalidity_change_resets_folder() {
        assert!(needs_reset(7, 8));
        assert!(!needs_reset(7, 7));
        assert!(!needs_reset(0, 8));
    }

    fn fake(reply: impl Fn(&str) -> Option<String> + Send + Sync + 'static) -> Conn {
        Conn::plain(TcpStream::connect(crate::imap::fake_server(reply)).unwrap(), "me", "pw").unwrap()
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

    /// uid 1 got flagged, uid 2 was deleted, uid 3 is a new big message: text + PDF.
    fn after(cmd: &str) -> Option<String> {
        let alt = "((\"text\" \"plain\" (\"charset\" \"utf-8\") NIL NIL \"7bit\" 9 1)(\"text\" \"html\" (\"charset\" \"utf-8\") NIL NIL \"base64\" 40 1) \"alternative\")";
        let pdf = "(\"application\" \"pdf\" (\"name\" \"=?UTF-8?B?0KHRh9C10YIucGRm?=\") NIL NIL \"base64\" 4000 NIL (\"attachment\" (\"filename\" \"=?UTF-8?B?0KHRh9C10YIucGRm?=\")) NIL NIL)";
        let bs = format!("({alt}{pdf} \"mixed\")");
        Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "EXAMINE \"INBOX\"" => "* 2 EXISTS\r\n* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 4] n\r\n".into(),
            "UID SEARCH UID 3:*" | "UID SEARCH UID 4:*" => "* SEARCH 3\r\n".into(),
            "UID FETCH 3 (UID FLAGS RFC822.SIZE)" => "* 2 FETCH (UID 3 FLAGS () RFC822.SIZE 3000000)\r\n".into(),
            "UID FETCH 3 (UID BODY.PEEK[HEADER] BODYSTRUCTURE)" =>
                format!("* 2 FETCH (UID 3 BODY[HEADER] {} BODYSTRUCTURE {bs})\r\n", lit("Subject: big\r\nFrom: b@x.org\r\n\r\n")),
            "UID FETCH 3 (UID BODY.PEEK[1.1] BODY.PEEK[1.2])" =>
                format!("* 2 FETCH (UID 3 BODY[1.1] {} BODY[1.2] {})\r\n", lit("big plain"), lit("PGI+YmlnPC9iPjxzY3JpcHQ+eDwvc2NyaXB0Pg==")),
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

        let mut ids = vec![];
        sync_account(&mut fake(before), &db, &acc, |v| ids.extend(v)).unwrap();
        assert_eq!(ids.len(), 2);
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
        let a: Vec<_> = big.attachments.iter().map(|a| (a.index, a.name.as_str(), a.mime.as_str(), a.size, a.inline)).collect();
        assert_eq!(a, [(0, "Счет.pdf", "application/pdf", 4000, false)]);
        assert!(big.summary.has_attachments);
        let mut subjects: Vec<_> = list().into_iter().map(|m| (m.subject, m.flagged)).collect();
        subjects.sort();
        assert_eq!(subjects, [("big".into(), false), ("one".into(), true)]);
        let f = inbox();
        assert_eq!((f.last_uid, f.notified_uid), (3, 2));

        assert!(sync_folder(&mut fake(after), &db, &f).unwrap().is_empty());
    }

    /// uid 2 answers with a line the parser rejects, in the batch and alone.
    fn garbled(cmd: &str) -> Option<String> {
        let each = |set: &str, f: &dyn Fn(u32) -> String| set.split(',').filter_map(|u| u.parse().ok()).map(f).collect::<String>();
        if let Some((set, query)) = cmd.strip_prefix("UID FETCH ").and_then(|r| r.split_once(' ')) {
            return Some(match query {
                "(UID FLAGS RFC822.SIZE)" => each(set, &|u| format!("* {u} FETCH (UID {u} FLAGS () RFC822.SIZE 60)\r\n")),
                "(UID BODY.PEEK[])" => each(set, &|u| match u {
                    2 => "* 2 FETCH (UID 2 BODY[] #garbage#)\r\n".into(),
                    _ => format!("* {u} FETCH (UID {u} BODY[] {})\r\n", lit(M1)),
                }),
                "(UID FLAGS)" => "* 1 FETCH (UID 1 FLAGS ())\r\n* 3 FETCH (UID 3 FLAGS ())\r\n".into(),
                _ => return None,
            });
        }
        Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "EXAMINE \"INBOX\"" => "* 3 EXISTS\r\n* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 4] n\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 1 2 3\r\n".into(),
            _ => return None,
        })
    }

    #[test]
    fn unparseable_message_is_skipped() {
        let db = Db::memory();
        db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let f = db.folders_of("me@x").unwrap().remove(0);
        assert_eq!(sync_folder(&mut fake(garbled), &db, &f).unwrap().len(), 2);
        assert_eq!(db.folders_of("me@x").unwrap()[0].last_uid, 3);
        assert_eq!(db.list(&ListQuery { folder: Some(f.id), ..Default::default() }).unwrap().len(), 2);
    }

    const GARBAGE: &str = "* 1 FETCH (UID 1 BODY[] #garbage#)\r\n";

    /// INBOX answers EXAMINE with a line the parser rejects; Work is fine.
    fn bad_examine(cmd: &str) -> Option<String> {
        Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n* LIST () \"/\" \"Work\"\r\n".into(),
            "EXAMINE \"INBOX\"" => GARBAGE.into(),
            "EXAMINE \"Work\"" => "* 1 EXISTS\r\n* OK [UIDVALIDITY 3] v\r\n* OK [UIDNEXT 2] n\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 1\r\n".into(),
            "UID FETCH 1 (UID FLAGS RFC822.SIZE)" => "* 1 FETCH (UID 1 FLAGS () RFC822.SIZE 60)\r\n".into(),
            "UID FETCH 1 (UID BODY.PEEK[])" => format!("* 1 FETCH (UID 1 BODY[] {})\r\n", lit(M1)),
            "UID FETCH 1:* (UID FLAGS)" => "* 1 FETCH (UID 1 FLAGS ())\r\n".into(),
            _ => return None,
        })
    }

    #[test]
    fn unparseable_examine_skips_to_next_folder() {
        let db = Db::memory();
        let s = Server { host: "h".into(), port: 1 };
        let acc = Account { email: "me@x".into(), name: "N".into(), provider: Provider::Imap, login: "me".into(), imap: s.clone(), smtp: s, append_sent: false };
        let mut ids = vec![];
        sync_account(&mut fake(bad_examine), &db, &acc, |v| ids.extend(v)).unwrap();
        assert_eq!(ids.len(), 1);
        let work = db.folders_of("me@x").unwrap().into_iter().find(|f| f.path == "Work").unwrap();
        assert_eq!(work.last_uid, 1);
    }

    static LOGINS: AtomicUsize = AtomicUsize::new(0);

    /// Every FETCH answer is unparseable.
    fn all_garbage(cmd: &str) -> Option<String> {
        Some(match cmd {
            c if c.starts_with("LOGIN") => {
                LOGINS.fetch_add(1, Ordering::SeqCst);
                String::new()
            }
            "EXAMINE \"INBOX\"" => "* 10 EXISTS\r\n* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 11] n\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 1 2 3 4 5 6 7 8 9 10\r\n".into(),
            c if c.starts_with("UID FETCH") => GARBAGE.into(),
            _ => return None,
        })
    }

    #[test]
    fn skips_are_capped() {
        let db = Db::memory();
        db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let f = db.folders_of("me@x").unwrap().remove(0);
        let e = sync_folder(&mut fake(all_garbage), &db, &f).unwrap_err();
        assert!(e.starts_with(UNPARSEABLE), "{e}");
        // the first login plus one reconnect per skip and one after the last skip
        assert_eq!(LOGINS.load(Ordering::SeqCst), 1 + MAX_SKIPS + 1);
        assert_eq!(db.folders_of("me@x").unwrap()[0].last_uid, MAX_SKIPS as u32);
    }

    /// EXAMINE without EXISTS; both local messages are still on the server.
    fn no_exists(cmd: &str) -> Option<String> {
        Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "EXAMINE \"INBOX\"" => "* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 3] n\r\n".into(),
            "UID SEARCH UID 3:*" => "* SEARCH 2\r\n".into(),
            "UID FETCH 1:* (UID FLAGS)" => "* 1 FETCH (UID 1 FLAGS ())\r\n* 2 FETCH (UID 2 FLAGS ())\r\n".into(),
            _ => return None,
        })
    }

    #[test]
    fn missing_exists_keeps_messages() {
        let db = Db::memory();
        let id = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        db.reset_folder(id, 7).unwrap();
        for uid in 1..=2 { db.insert_message(id, uid, false, false, 1, &mime::parse(M1.as_bytes()), true).unwrap(); }
        db.set_folder_marks(id, 2, 2).unwrap();
        let f = db.folders_of("me@x").unwrap().remove(0);
        assert!(sync_folder(&mut fake(no_exists), &db, &f).unwrap().is_empty());
        assert_eq!(db.list(&ListQuery { folder: Some(id), ..Default::default() }).unwrap().len(), 2);
    }

    const BACKEND: &str = "NO [UNAVAILABLE] UID FETCH message 2 uid 2: failure due to backend error";

    /// INBOX holds uids 1..=3; while `broken`, any FETCH that includes uid 2 gets a tagged NO.
    fn yandex(broken: bool, flags_no: bool) -> impl Fn(&str) -> Option<String> + Send + Sync + 'static {
        move |cmd| {
            let each = |set: &str, f: &dyn Fn(u32) -> String| set.split(',').filter_map(|u| u.parse().ok()).map(f).collect::<String>();
            if let Some((set, query)) = cmd.strip_prefix("UID FETCH ").and_then(|r| r.split_once(' ')) {
                if query == "(UID FLAGS)" {
                    return Some(if flags_no { BACKEND.into() } else { format!("* 1 FETCH (UID 1 FLAGS ())\r\n{}* 3 FETCH (UID 3 FLAGS ())\r\n", if broken { "" } else { "* 2 FETCH (UID 2 FLAGS ())\r\n" }) });
                }
                if broken && set.split(',').any(|u| u == "2") {
                    return Some(BACKEND.into());
                }
                return Some(match query {
                    "(UID FLAGS RFC822.SIZE)" => each(set, &|u| format!("* {u} FETCH (UID {u} FLAGS () RFC822.SIZE 60)\r\n")),
                    "(UID BODY.PEEK[])" => each(set, &|u| format!("* {u} FETCH (UID {u} BODY[] {})\r\n", lit(if u == 2 { M2 } else { M1 }))),
                    _ => return None,
                });
            }
            Some(match cmd {
                c if c.starts_with("LOGIN") => String::new(),
                "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
                "EXAMINE \"INBOX\"" => "* 3 EXISTS\r\n* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 4] n\r\n".into(),
                "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 1 2 3\r\n".into(),
                "UID SEARCH UID 3:*" | "UID SEARCH UID 4:*" => "* SEARCH 3\r\n".into(),
                _ => return None,
            })
        }
    }

    fn acc() -> Account {
        let s = Server { host: "h".into(), port: 1 };
        Account { email: "me@x".into(), name: "N".into(), provider: Provider::Imap, login: "me".into(), imap: s.clone(), smtp: s, append_sent: false }
    }

    fn inbox(db: &Db) -> Folder { db.folders_of("me@x").unwrap().remove(0) }

    #[test]
    fn refused_message_is_skipped_then_retried() {
        let db = Db::memory();
        let mut ids = vec![];
        sync_account(&mut fake(yandex(true, false)), &db, &acc(), |v| ids.extend(v)).unwrap();
        let f = inbox(&db);
        assert_eq!((ids.len(), f.last_uid), (2, 3));
        assert_eq!(db.refused_uids(f.id).unwrap(), [2]);

        // the server delivers it now
        let mut ids = vec![];
        sync_account(&mut fake(yandex(false, false)), &db, &acc(), |v| ids.extend(v)).unwrap();
        assert_eq!(ids.len(), 1);
        assert!(db.refused_uids(f.id).unwrap().is_empty());
        let subjects: Vec<_> = db.list(&ListQuery { folder: Some(f.id), ..Default::default() }).unwrap().into_iter().map(|m| m.subject).collect();
        assert_eq!(subjects.len(), 3);
        assert!(subjects.contains(&"two".to_string()));
    }

    #[test]
    fn refused_message_is_given_up_after_20_tries() {
        let db = Db::memory();
        let mut c = fake(yandex(true, false));
        sync_account(&mut c, &db, &acc(), |_| {}).unwrap();
        let f = inbox(&db);
        for _ in 1..MAX_TRIES {
            sync_account(&mut c, &db, &acc(), |_| {}).unwrap();
            assert_eq!(db.refused_uids(f.id).unwrap(), [2]);
        }
        sync_account(&mut c, &db, &acc(), |_| {}).unwrap();
        assert!(db.refused_uids(f.id).unwrap().is_empty());
    }

    #[test]
    fn refused_flags_step_keeps_everything() {
        let db = Db::memory();
        let id = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        db.reset_folder(id, 7).unwrap();
        for uid in 1..=2 { db.insert_message(id, uid, false, false, 1, &mime::parse(M1.as_bytes()), true).unwrap(); }
        db.set_folder_marks(id, 2, 2).unwrap();
        let f = inbox(&db);
        assert!(sync_folder(&mut fake(yandex(false, true)), &db, &f).unwrap().len() == 1);
        assert_eq!(db.list(&ListQuery { folder: Some(id), ..Default::default() }).unwrap().len(), 3);
    }

    fn tries(db: &Db, folder: i64) -> Vec<(u32, u32)> {
        let mut st = db.c.prepare("SELECT uid, tries FROM refused WHERE folder = ?1 ORDER BY uid").unwrap();
        st.query_map([folder], |r| Ok((r.get(0)?, r.get(1)?))).unwrap().map(Result::unwrap).collect()
    }

    /// Every FETCH of messages is answered with NO: an outage, not single messages.
    fn all_no(cmd: &str) -> Option<String> {
        if cmd.starts_with("UID FETCH ") && !cmd.ends_with("(UID FLAGS)") { return Some(BACKEND.into()) }
        yandex(false, false)(cmd)
    }

    #[test]
    fn refusing_everything_is_an_error_without_rows() {
        let db = Db::memory();
        db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let f = inbox(&db);
        let e = sync_folder(&mut fake(all_no), &db, &f).unwrap_err();
        assert!(e.starts_with(REFUSED), "{e}");
        assert_eq!((inbox(&db).last_uid, db.refused_uids(f.id).unwrap()), (0, vec![]));
    }

    #[test]
    fn retry_pass_stops_after_three_refusals() {
        let db = Db::memory();
        let id = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        for u in 1..=6 { db.add_refused(id, u).unwrap(); }
        let old: Vec<u32> = (1..=6).collect();
        let outage = |cmd: &str| if cmd.starts_with("UID FETCH") { Some(BACKEND.into()) } else { yandex(true, false)(cmd) };
        let mut c = fake(outage);
        retry_refused(&mut c, &db, &inbox(&db), &old, &mut vec![]).unwrap();
        assert_eq!(tries(&db, id), (1..=6).map(|u| (u, 0)).collect::<Vec<_>>());
    }

    #[test]
    fn failed_retry_keeps_new_ids() {
        let db = Db::memory();
        let id = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        db.reset_folder(id, 7).unwrap();
        db.set_folder_marks(id, 2, 2).unwrap();
        db.add_refused(id, 2).unwrap();
        let odd = |cmd: &str| if cmd == "UID FETCH 2 (UID BODY.PEEK[])" { Some("* 2 FETCH (UID 2 BODY[] #garbage#)\r\n".into()) } else { yandex(false, false)(cmd) };
        let mut ids = vec![];
        sync_account(&mut fake(odd), &db, &acc(), |v| ids.extend(v)).unwrap();
        assert_eq!(ids.len(), 1);
    }

    #[test]
    fn refusal_bound_stops_the_folder() {
        // every even uid is refused, the odd ones are fine
        let many = |cmd: &str| {
            let (set, query) = cmd.strip_prefix("UID FETCH ")?.split_once(' ')?;
            let uids: Vec<u32> = set.split(',').filter_map(|u| u.parse().ok()).collect();
            if uids.iter().any(|u| u % 2 == 0) { return Some(BACKEND.into()) }
            match query {
                "(UID FLAGS RFC822.SIZE)" => Some(uids.iter().map(|u| format!("* {u} FETCH (UID {u} FLAGS () RFC822.SIZE 60)\r\n")).collect()),
                "(UID BODY.PEEK[])" => Some(uids.iter().map(|u| format!("* {u} FETCH (UID {u} BODY[] {})\r\n", lit(M1))).collect()),
                _ => None,
            }
        };
        let server = move |cmd: &str| match cmd {
            c if c.starts_with("LOGIN") => Some(String::new()),
            "EXAMINE \"INBOX\"" => Some("* OK [UIDVALIDITY 7] v\r\n* OK [UIDNEXT 1000] n\r\n".into()),
            "UID SEARCH SINCE 1-Jan-2026" => Some(format!("* SEARCH {}\r\n", (1..=450).map(|u| u.to_string()).collect::<Vec<_>>().join(" "))),
            c => many(c),
        };
        let db = Db::memory();
        let id = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let e = sync_folder(&mut fake(server), &db, &inbox(&db)).unwrap_err();
        assert!(e.contains("gave up after 200"), "{e}");
        assert_eq!(db.refused_uids(id).unwrap().len(), MAX_REFUSED);
    }

    #[test]
    fn refused_uid_gone_from_server_leaves_the_table() {
        let db = Db::memory();
        let id = db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        db.add_refused(id, 9).unwrap();
        let gone = |cmd: &str| if cmd.starts_with("UID FETCH 9 ") { Some(String::new()) } else { yandex(false, false)(cmd) };
        retry_refused(&mut fake(gone), &db, &inbox(&db), &[9], &mut vec![]).unwrap();
        assert!(db.refused_uids(id).unwrap().is_empty());
    }

    #[test]
    fn refusal_does_not_log_in_again() {
        let logins = std::sync::Arc::new(AtomicUsize::new(0));
        let n = logins.clone();
        let counting = move |cmd: &str| {
            if cmd.starts_with("LOGIN") { n.fetch_add(1, Ordering::SeqCst); }
            yandex(true, false)(cmd)
        };
        let db = Db::memory();
        sync_account(&mut fake(counting), &db, &acc(), |_| {}).unwrap();
        assert_eq!(logins.load(Ordering::SeqCst), 1);
    }
}
