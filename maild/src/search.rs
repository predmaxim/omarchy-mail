//! Server-side search over mail older than the local history (before `sync::SINCE`), and opening
//! a hit: the worker's `Job::Search` and `Job::FetchRemote` run `search` and `open`.

use crate::config::Account;
use crate::db::{Db, Remote, Role, Summary};
use crate::i18n::tr;
use crate::imap::Conn;
use crate::mime;
use crate::sync::SINCE;
use ::imap::types::Flag;

/// Longest query, in characters.
const MAX_QUERY: usize = 256;
/// Hits per account.
const LIMIT: usize = 30;
/// Opened hits live in a folder of their own per source folder; its path can clash with no real one.
const REMOTE_PREFIX: &str = "@remote/";

/// An IMAP string: quoted, or for 8-bit text a non-synchronizing literal (`literals`: the server
/// has LITERAL+ or LITERAL-). The imap crate cannot wait for a `+`, so without them the text goes
/// in a quoted string as UTF-8, which most servers take after `CHARSET UTF-8`.
fn arg(q: &str, literals: bool) -> String {
    if literals && !q.is_ascii() {
        format!("{{{}+}}\r\n{q}", q.len())
    } else {
        format!("\"{}\"", q.replace('\\', "\\\\").replace('"', "\\\""))
    }
}

/// Control characters become spaces; at most `MAX_QUERY` characters (a literal stays far below
/// the 4096 bytes LITERAL- allows, even three times in one command).
fn clean(q: &str) -> String {
    q.chars().map(|c| if c.is_control() { ' ' } else { c }).collect::<String>().trim().chars().take(MAX_QUERY).collect::<String>().trim().to_string()
}

/// The `UID SEARCH` criteria for `q` among mail before `SINCE`: subject, sender or text.
pub fn criteria(q: &str, literals: bool) -> String {
    let q = clean(q);
    let a = arg(&q, literals);
    debug_assert!(a.len() <= 4096 + 16);
    let charset = if q.is_ascii() { "" } else { "CHARSET UTF-8 " };
    format!("{charset}BEFORE {SINCE} OR OR SUBJECT {a} FROM {a} TEXT {a}")
}

/// Where to look: Gmail's `\All` only, otherwise every folder but Junk.
fn folders(found: Vec<(String, Role)>, all: Option<String>) -> Vec<String> {
    match all {
        Some(a) => vec![a],
        None => found.into_iter().filter(|f| f.1 != Role::Junk).map(|f| f.0).collect(),
    }
}

fn one_folder(conn: &mut Conn, acc: &Account, path: &str, crit: &str, hits: &mut Vec<Summary>) -> Result<(), String> {
    conn.examine(path)?;
    let uids = conn.uids(crit)?;
    let newest = &uids[uids.len().saturating_sub(LIMIT)..];
    if newest.is_empty() {
        return Ok(());
    }
    let set = newest.iter().map(u32::to_string).collect::<Vec<_>>().join(",");
    for f in conn.fetch(&set, "(UID FLAGS INTERNALDATE BODY.PEEK[HEADER])")?.iter() {
        let Some(uid) = f.uid else { continue };
        let p = mime::parse(f.header().unwrap_or_default());
        hits.push(Summary {
            id: 0, account: acc.email.clone(), role: Role::Remote, folder_path: path.to_string(),
            date: if p.date != 0 { p.date } else { f.internal_date().map_or(0, |d| d.timestamp()) },
            from: p.from, subject: p.subject, preview: String::new(),
            seen: f.flags().contains(&Flag::Seen), flagged: f.flags().contains(&Flag::Flagged),
            has_attachments: false, queued: false,
            remote: Some(Remote { account: acc.email.clone(), folder_path: path.to_string(), uid }),
        });
    }
    Ok(())
}

/// Up to `LIMIT` newest hits of the account, and the first folder error if some folder failed
/// (the others still count). A dead connection is an `Err`.
pub fn search(conn: &mut Conn, acc: &Account, q: &str) -> Result<(Vec<Summary>, Option<String>), String> {
    let q = clean(q);
    if q.is_empty() {
        return Ok((vec![], None));
    }
    let literals = !q.is_ascii() && conn.capabilities().map_err(String::from)?.iter().any(|c| c == "LITERAL+" || c == "LITERAL-");
    let crit = criteria(&q, literals);
    let (found, all) = conn.discover()?;
    let mut hits = vec![];
    let mut failed = None;
    for path in folders(found, all) {
        if let Err(e) = one_folder(conn, acc, &path, &crit, &mut hits) {
            if conn.noop().is_err() {
                return Err(e);
            }
            eprintln!("{}: search in {path}: {e}", acc.email);
            failed.get_or_insert(e);
        }
    }
    hits.sort_by_key(|h| std::cmp::Reverse(h.date));
    hits.truncate(LIMIT);
    Ok((hits, failed))
}

/// Downloads a hit into the account's remote folder for its source folder; the local id. Opening
/// it again refreshes the same row.
pub fn open(conn: &mut Conn, db: &Db, acc: &str, path: &str, uid: u32) -> Result<i64, String> {
    let gone = || tr("message is gone from the server", "письма уже нет на сервере");
    conn.examine(path)?;
    let got = conn.fetch(&uid.to_string(), "(UID FLAGS BODY.PEEK[])")?;
    let f = got.iter().find(|f| f.uid == Some(uid)).ok_or_else(gone)?;
    let raw = f.body().ok_or_else(gone)?;
    let folder = db.upsert_folder(acc, &format!("{REMOTE_PREFIX}{path}"), Role::Remote)?;
    let p = mime::parse(raw);
    db.insert_message(folder, uid, f.flags().contains(&Flag::Seen), f.flags().contains(&Flag::Flagged), raw.len() as u64, &p, true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Provider, Server};
    use crate::hub::{Event, Hub};
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::{SocketAddr, TcpListener, TcpStream};
    use std::sync::{Arc, Mutex};

    #[test]
    fn criteria_escapes() {
        assert_eq!(arg("a\"b\\c", true), r#""a\"b\\c""#);
        assert_eq!(criteria("a\"b\r\n", true), r#"BEFORE 1-Jan-2026 OR OR SUBJECT "a\"b" FROM "a\"b" TEXT "a\"b""#);
    }

    #[test]
    fn criteria_utf8() {
        assert_eq!(criteria("до", true), "CHARSET UTF-8 BEFORE 1-Jan-2026 OR OR SUBJECT {4+}\r\nдо FROM {4+}\r\nдо TEXT {4+}\r\nдо");
        assert_eq!(criteria("до", false), "CHARSET UTF-8 BEFORE 1-Jan-2026 OR OR SUBJECT \"до\" FROM \"до\" TEXT \"до\"");
    }

    #[test]
    fn stale_token_keeps_its_own() {
        let hub = Hub::default();
        let rx = hub.subscribe(true);
        hub.emit(Event::SearchRemote { token: 1, hits: vec![], error: None });
        hub.emit(Event::SearchRemote { token: 2, hits: vec![], error: Some("x".into()) });
        assert_eq!(rx.recv().unwrap(), r#"{"event":"search_remote","token":1,"hits":[]}"#);
        assert_eq!(rx.recv().unwrap(), r#"{"event":"search_remote","token":2,"hits":[],"error":"x"}"#);
    }

    /// IMAP server that, unlike `fake_server`, handles literals: answers `+` to `{n}` and reads
    /// the n bytes. Logs each command with literals inlined as `<text>`.
    fn server(list: &'static str, caps: &'static str, log: Arc<Mutex<Vec<String>>>) -> SocketAddr {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap();
        std::thread::spawn(move || {
            for s in l.incoming() {
                let Ok(s) = s else { break };
                let (log, mut w) = (log.clone(), s.try_clone().unwrap());
                std::thread::spawn(move || {
                    let mut r = BufReader::new(s);
                    let _ = w.write_all(b"* OK hi\r\n");
                    let mut cmd = String::new();
                    loop {
                        let mut line = String::new();
                        if r.read_line(&mut line).unwrap_or(0) == 0 { break }
                        let line = line.trim_end();
                        let lit = line.strip_suffix('}').and_then(|l| l.rsplit_once('{')).and_then(|(head, n)| Some((head, n.strip_suffix('+').unwrap_or(n).parse::<usize>().ok()?, n.ends_with('+'))));
                        if let Some((head, n, plus)) = lit {
                            if !plus { w.write_all(b"+ go\r\n").unwrap(); }
                            let mut b = vec![0; n];
                            r.read_exact(&mut b).unwrap();
                            cmd += &format!("{head}<{}>", String::from_utf8_lossy(&b));
                            continue;
                        }
                        cmd += line;
                        let (tag, c) = cmd.split_once(' ').unwrap_or((&cmd, ""));
                        log.lock().unwrap().push(c.to_string());
                        let hdr = "From: =?UTF-8?B?0JjQstCw0L0=?= <i@x.org>\r\nSubject: =?UTF-8?B?0LTQvtCz0L7QstC+0YA=?=\r\nDate: Mon, 2 Jun 2025 10:00:00 +0000\r\n\r\n";
                        let body = format!("{hdr}hello");
                        let reply = match c.split_whitespace().next().unwrap_or("") {
                            "LIST" => list.to_string(),
                            "CAPABILITY" => format!("* CAPABILITY IMAP4rev1 {caps}\r\n"),
                            "EXAMINE" if c.contains("Broken") => {
                                let _ = w.write_all(format!("{tag} NO [NONEXISTENT] nope\r\n").as_bytes());
                                cmd.clear();
                                continue;
                            }
                            "EXAMINE" => "* 0 EXISTS\r\n".into(),
                            "UID" if c.starts_with("UID SEARCH") => "* SEARCH 4 7\r\n".into(),
                            "UID" if c.contains("BODY.PEEK[]") => format!("* 1 FETCH (UID 7 FLAGS (\\Seen) BODY[] {{{}}}\r\n{body})\r\n", body.len()),
                            "UID" => format!("* 1 FETCH (UID 7 FLAGS (\\Seen) INTERNALDATE \"02-Jun-2025 10:00:00 +0000\" BODY[HEADER] {{{}}}\r\n{hdr})\r\n", hdr.len()),
                            _ => String::new(),
                        };
                        let _ = w.write_all(format!("{reply}{tag} OK done\r\n").as_bytes());
                        cmd.clear();
                    }
                });
            }
        });
        addr
    }

    fn acc() -> Account {
        let s = Server { host: "h".into(), port: 1 };
        Account { email: "me@x".into(), name: "N".into(), provider: Provider::Imap, login: "me".into(), imap: s.clone(), smtp: s, append_sent: false }
    }

    fn conn(addr: SocketAddr) -> Conn {
        Conn::plain(TcpStream::connect(addr).unwrap(), "me", "pw").unwrap()
    }

    #[test]
    fn gmail_searches_all_mail_only_with_literal() {
        let log = Arc::new(Mutex::new(vec![]));
        let addr = server("* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n* LIST (\\All) \"/\" \"[Gmail]/All Mail\"\r\n", "LITERAL+", log.clone());
        let (hits, failed) = search(&mut conn(addr), &acc(), "договор").unwrap();
        assert!(failed.is_none());
        let log = log.lock().unwrap();
        assert!(log.contains(&"EXAMINE \"[Gmail]/All Mail\"".to_string()) && !log.contains(&"EXAMINE \"INBOX\"".to_string()), "{log:?}");
        assert!(log.iter().any(|c| c == "UID SEARCH CHARSET UTF-8 BEFORE 1-Jan-2026 OR OR SUBJECT <договор> FROM <договор> TEXT <договор>"), "{log:?}");
        let h = &hits[0];
        assert_eq!((h.id, h.role, h.folder_path.as_str(), h.subject.as_str(), h.from.name.as_str(), h.seen), (0, Role::Remote, "[Gmail]/All Mail", "договор", "Иван", true));
        let r = h.remote.as_ref().unwrap();
        assert_eq!((r.account.as_str(), r.folder_path.as_str(), r.uid), ("me@x", "[Gmail]/All Mail", 7));
    }

    #[test]
    fn other_servers_skip_junk_and_open_is_idempotent() {
        let log = Arc::new(Mutex::new(vec![]));
        let addr = server("* LIST () \"/\" \"INBOX\"\r\n* LIST (\\Junk) \"/\" \"Spam\"\r\n* LIST (\\Trash) \"/\" \"Trash\"\r\n", "", log.clone());
        let mut c = conn(addr);
        let hits = search(&mut c, &acc(), "a b").unwrap().0;
        let exam: Vec<String> = log.lock().unwrap().iter().filter(|c| c.starts_with("EXAMINE")).cloned().collect();
        assert_eq!(exam, ["EXAMINE \"INBOX\"", "EXAMINE \"Trash\""]);
        assert_eq!(hits.len(), 2);
        // no literals: UTF-8 goes in a quoted string
        search(&mut c, &acc(), "до").unwrap();
        assert!(log.lock().unwrap().iter().any(|c| c.ends_with("TEXT \"до\"")));

        let db = Db::memory();
        let id = open(&mut c, &db, "me@x", "INBOX", 7).unwrap();
        assert_eq!(open(&mut c, &db, "me@x", "INBOX", 7).unwrap(), id);
        let m = db.message(id).unwrap().unwrap();
        assert_eq!((m.summary.role, m.summary.subject.as_str(), m.text.trim()), (Role::Remote, "договор", "hello"));
        assert!(open(&mut c, &db, "me@x", "INBOX", 99).is_err());
    }

    #[test]
    fn failed_folder_keeps_other_hits() {
        let log = Arc::new(Mutex::new(vec![]));
        let addr = server("* LIST () \"/\" \"INBOX\"\r\n* LIST (\\Trash) \"/\" \"Broken\"\r\n", "", log);
        let (hits, failed) = search(&mut conn(addr), &acc(), "x").unwrap();
        assert_eq!(hits.len(), 1);
        assert!(failed.unwrap().contains("nope"));
    }

    #[test]
    fn query_is_cleaned_and_capped() {
        assert_eq!(clean(" a\u{1}b "), "a b");
        assert_eq!(clean(&"я".repeat(1000)).chars().count(), MAX_QUERY);
        let log = Arc::new(Mutex::new(vec![]));
        let addr = server("* LIST () \"/\" \"INBOX\"\r\n", "", log.clone());
        let (h, e) = search(&mut conn(addr), &acc(), "\u{1}\n\t").unwrap();
        assert!(h.is_empty() && e.is_none());
        assert!(log.lock().unwrap().iter().all(|c| !c.contains("SEARCH")), "searched an empty query");
    }
}
