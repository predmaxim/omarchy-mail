//! Sending: the draft becomes a message in the `outbox` table at once; one thread delivers the
//! queued rows over SMTP and retries on network errors.

use crate::account::Job;
use crate::config::{self, Account, Paths};
use crate::db::{Db, Full, Role};
use crate::hub::{Event, Hub, lock};
use crate::i18n::tr;
use crate::mime::Addr;
use crate::secret;
use lettre::message::{Attachment, Mailbox, MultiPart, SinglePart, header::ContentType};
use lettre::transport::smtp::authentication::Credentials;
use lettre::{Address, Message, SmtpTransport, Transport, address::Envelope};
use serde::Deserialize;
use std::path::PathBuf;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender, channel};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// How long a row waits after a network error.
const RETRY: Duration = Duration::from_secs(30);

/// The `send` request. `attachments` are absolute paths on this machine.
#[derive(Debug, Clone, Deserialize)]
pub struct Draft {
    pub account: String,
    pub to: Vec<Addr>,
    pub subject: String,
    pub text: String,
    #[serde(default)]
    pub reply_to: Option<i64>,
    #[serde(default)]
    pub attachments: Vec<PathBuf>,
}

fn mailbox(a: &Addr) -> Result<Mailbox, String> {
    let addr: Address = a.addr.trim().parse().map_err(|e| format!("{}: {e}", a.addr))?;
    Ok(Mailbox::new((!a.name.is_empty()).then(|| a.name.clone()), addr))
}

/// `<id>` from `id` or `<id>`; `None` if empty.
fn bracketed(id: &str) -> Option<String> {
    let id = id.trim().trim_start_matches('<').trim_end_matches('>');
    (!id.is_empty()).then(|| format!("<{id}>"))
}

fn mime_of(path: &std::path::Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).map(str::to_ascii_lowercase).as_deref() {
        Some("png") => "image/png",
        Some("jpg" | "jpeg") => "image/jpeg",
        Some("gif") => "image/gif",
        Some("webp") => "image/webp",
        Some("pdf") => "application/pdf",
        Some("txt") => "text/plain",
        Some("zip") => "application/zip",
        Some("doc") => "application/msword",
        Some("docx") => "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        Some("xls") => "application/vnd.ms-excel",
        Some("xlsx") => "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        Some("odt") => "application/vnd.oasis.opendocument.text",
        Some("ods") => "application/vnd.oasis.opendocument.spreadsheet",
        _ => "application/octet-stream",
    }
}

fn new_message_id(email: &str) -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_nanos());
    let domain = email.rsplit('@').next().unwrap_or("localhost");
    format!("<{nanos}.{}.{}@{domain}>", std::process::id(), COUNTER.fetch_add(1, Relaxed))
}

/// The RFC 5322 message. `reply` is the message `d.reply_to` points at.
pub fn build(acc: &Account, d: &Draft, reply: Option<&Full>) -> Result<Vec<u8>, String> {
    if d.to.is_empty() {
        return Err(tr("no recipients", "нет получателей"));
    }
    let mut b = Message::builder()
        .from(mailbox(&Addr { name: acc.name.clone(), addr: acc.email.clone() })?)
        .subject(d.subject.clone())
        .date_now()
        .message_id(Some(new_message_id(&acc.email)));
    for t in &d.to {
        b = b.to(mailbox(t)?);
    }
    if let Some(r) = reply {
        let id = bracketed(&r.message_id);
        if let Some(id) = &id {
            b = b.in_reply_to(id.clone());
        }
        let refs: Vec<String> = r.refs.split_whitespace().filter_map(bracketed).chain(id).collect();
        if !refs.is_empty() {
            b = b.references(refs.join(" "));
        }
    }
    let text = SinglePart::plain(d.text.clone());
    let msg = if d.attachments.is_empty() {
        b.singlepart(text)
    } else {
        let mut parts = MultiPart::mixed().singlepart(text);
        for p in &d.attachments {
            let bytes = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
            let name = p.file_name().map_or_else(|| "file".into(), |n| n.to_string_lossy().into_owned());
            let ct = ContentType::parse(mime_of(p)).map_err(|e| e.to_string())?;
            parts = parts.singlepart(Attachment::new(name).body(bytes, ct));
        }
        b.multipart(parts)
    };
    msg.map(|m| m.formatted()).map_err(|e| e.to_string())
}

/// Builds and queues; the id is what `list` shows negated.
pub fn queue(db: &Db, acc: &Account, d: &Draft) -> Result<i64, String> {
    let reply = match d.reply_to {
        Some(id) => Some(db.message(id)?.ok_or_else(|| tr("message not found", "письмо не найдено"))?),
        None => None,
    };
    let raw = build(acc, d, reply.as_ref())?;
    let date = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |t| t.as_secs() as i64);
    db.outbox_add(&acc.email, &raw, &d.to, &d.subject, date)
}

/// Why a delivery failed.
#[derive(Debug, PartialEq)]
pub enum Fail {
    /// Network, TLS, 4xx: try again later.
    Retry(String),
    /// 5xx, a refused login or bad addresses: retrying cannot help.
    Permanent(String),
}

fn classify(e: &lettre::transport::smtp::Error) -> Fail {
    let m = e.to_string();
    if e.is_permanent() { Fail::Permanent(m) } else { Fail::Retry(m) }
}

/// Sends `raw` through `t` from `from` to `to`.
fn send_via(t: &SmtpTransport, from: &str, to: &[Addr], raw: &[u8]) -> Result<(), Fail> {
    let bad = |e: lettre::address::AddressError| Fail::Permanent(e.to_string());
    let rcpt = to.iter().map(|a| a.addr.trim().parse::<Address>().map_err(bad)).collect::<Result<Vec<_>, _>>()?;
    let env = Envelope::new(from.parse::<Address>().ok(), rcpt).map_err(|e| Fail::Permanent(e.to_string()))?;
    t.send_raw(&env, raw).map(drop).map_err(|e| classify(&e))
}

/// The real delivery: the account's server, implicit TLS, the keyring password.
fn deliver_smtp(acc: &Account, to: &[Addr], raw: &[u8]) -> Result<(), Fail> {
    let password = secret::lookup(&acc.email).map_err(Fail::Retry)?;
    let t = SmtpTransport::relay(&acc.smtp.host).map_err(|e| Fail::Retry(e.to_string()))?
        .port(acc.smtp.port)
        .credentials(Credentials::new(acc.login.clone(), password))
        .timeout(Some(Duration::from_secs(60)))
        .build();
    send_via(&t, &acc.email, to, raw)
}

type Deliver = dyn Fn(&Account, &[Addr], &[u8]) -> Result<(), Fail>;

/// One walk over the queued rows. True if some are left for a later try.
fn pass(db: &Db, hub: &Hub, accounts: &[Account], deliver: &Deliver) -> Result<bool, String> {
    let (mut blocked, mut left) = (vec![], false);
    for (id, account, to, raw) in db.outbox_queued()? {
        // A row waits behind an earlier one of its account that has a network problem.
        if blocked.contains(&account) {
            left = true;
            continue;
        }
        let Some(acc) = accounts.iter().find(|a| a.email == account) else { continue };
        match deliver(acc, &to, &raw) {
            Ok(()) => {
                // First of all: a row the server has accepted is never `queued` again, whatever fails next.
                // ponytail: a crash right before this update still sends the row again; no way to know.
                if let Err(e) = db.outbox_sent(id) {
                    eprintln!("{account}: send #{id}: marking sent: {e}");
                }
                // The copy for the Sent folder (servers that file nothing themselves) is filed by the
                // worker, which then drops the row; until then it stays in Sent.
                if acc.append_sent {
                    if let Some(h) = hub.handles().into_iter().find(|h| h.email == account) {
                        let _ = h.jobs.send(Job::Append { outbox: id, role: Role::Sent, raw });
                    }
                } else if let Err(e) = db.outbox_done(id) {
                    eprintln!("{account}: send #{id}: {e}");
                }
                hub.emit(Event::Changed { folders: true, ids: vec![] });
            }
            Err(Fail::Retry(e)) => {
                eprintln!("{account}: send #{id}: {e}");
                blocked.push(account);
                left = true;
            }
            Err(Fail::Permanent(error)) => {
                eprintln!("{account}: send #{id} refused: {error}");
                db.outbox_fail(id, &error)?;
                hub.emit(Event::SendFailed { outbox: id, error });
                hub.emit(Event::Changed { folders: false, ids: vec![] });
            }
        }
    }
    Ok(left)
}

/// Files one sent mail with `append` (the Sent folder of its account); the row goes after that.
pub fn append_row(db: &Db, id: i64, raw: &[u8], append: impl FnOnce(&[u8]) -> Result<(), String>) -> Result<(), String> {
    append(raw)?;
    db.outbox_done(id)
}

/// `append_row` for every sent mail of the account still unfiled, until one fails.
pub fn flush_unfiled(db: &Db, account: &str, mut append: impl FnMut(&[u8]) -> Result<(), String>) -> Result<(), String> {
    for (id, raw) in db.outbox_unfiled(account)? {
        append_row(db, id, &raw, &mut append)?;
    }
    Ok(())
}

static WAKE: Mutex<Option<Sender<()>>> = Mutex::new(None);

/// Tells the outbox thread there is a new row.
pub fn wake() {
    if let Some(tx) = &*lock(&WAKE) {
        let _ = tx.send(());
    }
}

/// Starts the outbox thread; it also sends what an earlier run left queued.
pub fn spawn(paths: Paths, hub: Hub) {
    let (tx, rx) = channel();
    *lock(&WAKE) = Some(tx);
    std::thread::spawn(move || run(&paths, &hub, &rx));
}

fn run(paths: &Paths, hub: &Hub, rx: &Receiver<()>) {
    let db = match Db::open(&paths.db) {
        Ok(db) => db,
        Err(e) => return eprintln!("maild: outbox: {e}"),
    };
    loop {
        let accounts = config::load(&paths.config).unwrap_or_default();
        let left = pass(&db, hub, &accounts, &deliver_smtp).unwrap_or_else(|e| {
            eprintln!("maild: outbox: {e}");
            true
        });
        let gone = if left { matches!(rx.recv_timeout(RETRY), Err(RecvTimeoutError::Disconnected)) } else { rx.recv().is_err() };
        if gone {
            return;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Provider, Server};
    use std::io::{BufRead, BufReader, Write};
    use std::net::TcpListener;
    use std::sync::Arc;

    fn acc(append_sent: bool) -> Account {
        let s = Server { host: "h".into(), port: 1 };
        Account { email: "me@x.org".into(), name: "Me".into(), provider: Provider::Imap, login: "me".into(), imap: s.clone(), smtp: s, append_sent }
    }

    fn addr(a: &str) -> Addr {
        Addr { name: String::new(), addr: a.into() }
    }

    fn draft(subject: &str) -> Draft {
        Draft { account: "me@x.org".into(), to: vec![addr("you@y.org")], subject: subject.into(), text: "hi".into(), reply_to: None, attachments: vec![] }
    }

    fn text(raw: &[u8]) -> String {
        String::from_utf8_lossy(raw).replace("\r\n ", " ")
    }

    fn orig(message_id: &str, refs: &str) -> Full {
        let db = Db::memory();
        let f = db.upsert_folder("me@x.org", "INBOX", Role::Inbox).unwrap();
        let p = crate::mime::Parsed { subject: "Re: s".into(), message_id: message_id.into(), references: refs.split_whitespace().map(String::from).collect(), ..Default::default() };
        let id = db.insert_message(f, 1, true, false, 1, &p, true).unwrap();
        db.message(id).unwrap().unwrap()
    }

    #[test]
    fn build_reply_headers() {
        let raw = text(&build(&acc(false), &draft("Re: s"), Some(&orig("c@z", "a@z b@z"))).unwrap());
        assert!(raw.contains("Subject: Re: s\r\n"), "{raw}"); // not doubled, not added
        assert!(raw.contains("In-Reply-To: <c@z>\r\n"), "{raw}");
        assert!(raw.contains("References: <a@z> <b@z> <c@z>\r\n"), "{raw}");
        assert!(raw.contains("From: Me <me@x.org>\r\n"), "{raw}");
        assert!(raw.contains("Content-Type: text/plain; charset=utf-8"), "{raw}");
        assert!(raw.contains("Date: "), "{raw}");
        assert!(raw.contains("@x.org>\r\n"), "{raw}");
        let plain = text(&build(&acc(false), &draft("s"), Some(&orig("", ""))).unwrap());
        assert!(!plain.contains("In-Reply-To") && !plain.contains("References"), "{plain}");
    }

    #[test]
    fn message_ids_differ() {
        let id = |raw: Vec<u8>| text(&raw).lines().find(|l| l.starts_with("Message-ID:")).unwrap().to_string();
        assert_ne!(id(build(&acc(false), &draft("s"), None).unwrap()), id(build(&acc(false), &draft("s"), None).unwrap()));
    }

    #[test]
    fn no_recipients_err() {
        let mut d = draft("s");
        d.to.clear();
        assert!(build(&acc(false), &d, None).is_err());
    }

    #[test]
    fn build_attachment_types() {
        let dir = std::env::temp_dir().join(format!("maild-test-{}-smtp-att", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let (png, bin) = (dir.join("a.PNG"), dir.join("b.xyz"));
        std::fs::write(&png, [1, 2, 3]).unwrap();
        std::fs::write(&bin, [4]).unwrap();
        let mut d = draft("s");
        d.attachments = vec![png, bin];
        let raw = text(&build(&acc(false), &d, None).unwrap());
        assert!(raw.contains("Content-Type: image/png"), "{raw}");
        assert!(raw.contains("Content-Type: application/octet-stream"), "{raw}");
        assert!(raw.contains("multipart/mixed"), "{raw}");
        assert!(raw.contains("a.PNG"), "{raw}");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn missing_attachment_err() {
        let mut d = draft("s");
        d.attachments = vec!["/nonexistent/file.png".into()];
        let e = build(&acc(false), &d, None).unwrap_err();
        assert!(e.contains("/nonexistent/file.png"), "{e}");
    }

    #[test]
    fn queue_visible_in_sent() {
        let db = Db::memory();
        let id = queue(&db, &acc(false), &draft("hello")).unwrap();
        let l = db.list(&crate::db::ListQuery { role: Some(Role::Sent), ..Default::default() }).unwrap();
        assert_eq!((l.len(), l[0].id, l[0].subject.as_str(), l[0].queued, l[0].failed), (1, -id, "hello", true, false));
        assert_eq!(db.queued_count("me@x.org").unwrap(), 1);
        let mut d = draft("x");
        d.attachments = vec!["/nonexistent".into()];
        assert!(queue(&db, &acc(false), &d).is_err());
        assert_eq!(db.queued_count("me@x.org").unwrap(), 1, "nothing queued on error");
        let mut d = draft("x");
        d.reply_to = Some(999);
        assert!(queue(&db, &acc(false), &d).is_err());
    }

    /// A one-shot plain SMTP server that answers RCPT with `rcpt`.
    fn fake_smtp(rcpt: &'static str) -> u16 {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for s in l.incoming() {
                let Ok(s) = s else { break };
                let mut w = s.try_clone().unwrap();
                let _ = w.write_all(b"220 fake\r\n");
                let mut data = false;
                for line in BufReader::new(s).lines() {
                    let Ok(line) = line else { break };
                    let out = match line.as_str() {
                        _ if data && line == "." => { data = false; "250 queued\r\n".into() }
                        _ if data => continue,
                        l if l.starts_with("EHLO") => "250 fake\r\n".to_string(),
                        l if l.starts_with("RCPT") => format!("{rcpt}\r\n"),
                        "DATA" => { data = true; "354 go\r\n".into() }
                        "QUIT" => "221 bye\r\n".into(),
                        _ => "250 ok\r\n".into(),
                    };
                    if w.write_all(out.as_bytes()).is_err() { break }
                }
            }
        });
        port
    }

    fn plain_send(port: u16) -> Result<(), Fail> {
        let t = SmtpTransport::builder_dangerous("127.0.0.1").port(port).build();
        send_via(&t, "me@x.org", &[addr("you@y.org")], b"Subject: s\r\n\r\nhi\r\n")
    }

    #[test]
    fn classify_by_smtp_code() {
        assert_eq!(plain_send(fake_smtp("250 ok")), Ok(()));
        assert!(matches!(plain_send(fake_smtp("550 no such user")), Err(Fail::Permanent(_))));
        assert!(matches!(plain_send(fake_smtp("451 try later")), Err(Fail::Retry(_))));
        let dead = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
        assert!(matches!(plain_send(dead), Err(Fail::Retry(_))), "connection refused");
        let bad = SmtpTransport::builder_dangerous("127.0.0.1").build();
        assert!(matches!(send_via(&bad, "me@x.org", &[addr("not an address")], b""), Err(Fail::Permanent(_))));
    }

    #[test]
    fn pass_delivers_retries_and_fails() {
        let db = Db::memory();
        let hub = Hub::default();
        let events = hub.subscribe(false);
        let accounts = [acc(false)];
        let (a, b, c) = (queue(&db, &accounts[0], &draft("a")).unwrap(), queue(&db, &accounts[0], &draft("b")).unwrap(), queue(&db, &accounts[0], &draft("c")).unwrap());
        // b is refused for good, a is delivered, c waits for the network
        let calls = Arc::new(Mutex::new(vec![]));
        let seen = calls.clone();
        let deliver = move |_: &Account, _: &[Addr], raw: &[u8]| {
            let t = text(raw);
            lock(&seen).push(t.lines().find(|l| l.starts_with("Subject")).unwrap().to_string());
            if t.contains("Subject: a") { Ok(()) } else if t.contains("Subject: b") { Err(Fail::Permanent("550 no".into())) } else { Err(Fail::Retry("down".into())) }
        };
        assert!(pass(&db, &hub, &accounts, &deliver).unwrap(), "c is left");
        let l = db.list(&crate::db::ListQuery { role: Some(Role::Sent), ..Default::default() }).unwrap();
        let rows: Vec<(i64, bool)> = l.iter().map(|s| (s.id, s.failed)).collect();
        assert_eq!(rows, [(-c, false), (-b, true)], "a is gone, b stays failed");
        let ev: Vec<String> = events.try_iter().collect();
        assert!(ev.iter().any(|e| e.contains(&format!(r#""event":"send_failed","outbox":{b},"error":"550 no""#))), "{ev:?}");
        assert!(ev.iter().any(|e| e.contains(r#""event":"changed","folders":true"#)), "{ev:?}");
        // the next pass tries only c; behind a network error nothing else is tried
        lock(&calls).clear();
        assert!(pass(&db, &hub, &accounts, &deliver).unwrap());
        assert_eq!(*lock(&calls), ["Subject: c"]);
        let _ = a;
    }

    #[test]
    fn network_error_holds_later_rows_of_the_account() {
        let db = Db::memory();
        let accounts = [acc(false)];
        queue(&db, &accounts[0], &draft("first")).unwrap();
        queue(&db, &accounts[0], &draft("second")).unwrap();
        let n = Arc::new(Mutex::new(0));
        let k = n.clone();
        let deliver = move |_: &Account, _: &[Addr], _: &[u8]| { *lock(&k) += 1; Err(Fail::Retry("down".into())) };
        assert!(pass(&db, &Hub::default(), &accounts, &deliver).unwrap());
        assert_eq!(*lock(&n), 1);
    }

    #[test]
    fn sent_copy_survives_failed_append() {
        let db = Db::memory();
        let accounts = [acc(true)];
        let id = queue(&db, &accounts[0], &draft("keep")).unwrap();
        let calls = Arc::new(Mutex::new(0));
        let k = calls.clone();
        let deliver = move |_: &Account, _: &[Addr], _: &[u8]| { *lock(&k) += 1; Ok(()) };
        let sent = || db.list(&crate::db::ListQuery { role: Some(Role::Sent), ..Default::default() }).unwrap();
        // no worker to take the Append: the row stays, filed under Sent as a normal (not queued) mail
        assert!(!pass(&db, &Hub::default(), &accounts, &deliver).unwrap());
        let l = sent();
        assert_eq!((l.len(), l[0].id, l[0].queued, l[0].failed), (1, -id, false, false));
        // the APPEND fails once: the row is still there
        let n = Mutex::new(0);
        let flaky = |_: &[u8]| { *lock(&n) += 1; if *lock(&n) == 1 { Err("offline".to_string()) } else { Ok(()) } };
        assert!(flush_unfiled(&db, "me@x.org", flaky).is_err());
        assert_eq!(sent().len(), 1);
        // the next try files it and drops the row; SMTP was used exactly once
        flush_unfiled(&db, "me@x.org", flaky).unwrap();
        assert!(sent().is_empty());
        assert!(!pass(&db, &Hub::default(), &accounts, &deliver).unwrap());
        assert_eq!(*lock(&calls), 1);
    }

    #[test]
    fn sent_row_is_never_sent_again() {
        let db = Db::memory();
        let accounts = [acc(true)];
        queue(&db, &accounts[0], &draft("once")).unwrap();
        let n = Arc::new(Mutex::new(0));
        let k = n.clone();
        let deliver = move |_: &Account, _: &[Addr], _: &[u8]| { *lock(&k) += 1; Ok(()) };
        for _ in 0..3 {
            pass(&db, &Hub::default(), &accounts, &deliver).unwrap();
        }
        assert_eq!(*lock(&n), 1);
    }

    #[test]
    fn build_non_ascii_and_no_header_injection() {
        let dir = std::env::temp_dir().join(format!("maild-test-{}-smtp-cyr", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("отчёт.pdf");
        std::fs::write(&file, [1]).unwrap();
        let mut a = acc(false);
        a.name = "Иван Петров".into();
        let mut d = draft("Привет, мир");
        d.text = "Текст письма".into();
        d.to = vec![Addr { name: "Анна".into(), addr: "anna@y.org".into() }];
        d.attachments = vec![file];
        let raw = build(&a, &d, None).unwrap();
        let t = text(&raw);
        let head = &t[..t.find("\r\n\r\n").unwrap()];
        assert!(head.contains("Subject: =?utf-8?"), "{head}");
        assert!(head.contains("From: =?utf-8?"), "{head}");
        assert!(head.contains("To: =?utf-8?"), "{head}");
        assert!(t.contains("charset=utf-8"), "{t}");
        let parsed = crate::mime::parse(&raw);
        assert_eq!((parsed.subject.as_str(), parsed.from.name.as_str(), parsed.text.trim()), ("Привет, мир", "Иван Петров", "Текст письма"));
        assert_eq!(parsed.attachments[0].name, "отчёт.pdf");
        assert!(t.contains("filename*") || t.contains("filename=\"=?utf-8?"), "{t}");
        std::fs::remove_dir_all(dir).unwrap();

        let evil = draft("hi\r\nBcc: x@example.org");
        let t = text(&build(&acc(false), &evil, None).unwrap());
        let head = &t[..t.find("\r\n\r\n").unwrap()];
        assert!(!head.lines().any(|l| l.starts_with("Bcc:")), "{head}");
    }
}
