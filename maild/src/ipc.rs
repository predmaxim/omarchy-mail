//! Socket protocol (PROTOCOL.md): one JSON object per line. `dispatch` answers one request;
//! `listen` + `serve` run the socket, a thread per client.

use crate::account::{self, Handle, Job};
use crate::actions::{self, Op};
use crate::fetch;
use crate::config::{self, Account, Paths, Provider, Server};
use crate::db::{Db, ListQuery};
use crate::hub::{Event, Hub, lock};
use crate::i18n::tr;
use crate::imap::Conn;
use crate::secret;
use crate::smtp::{self, Draft};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::Shutdown;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender, channel};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// A longer request line gets an error and the client is dropped.
pub const MAX_LINE: usize = 1024 * 1024;
/// How long a client waits for the worker to fetch a message or attachment.
const FETCH_WAIT: Duration = if cfg!(test) { Duration::from_millis(300) } else { Duration::from_secs(30) };
const SEARCH_LIMIT: u32 = 200;
const CONTACTS_LIMIT: u32 = 10;
/// A client that does not read for this long is dropped (its events would pile up).
const WRITE_TIMEOUT: Duration = Duration::from_secs(10);
/// How soon a subscription ends after its client hangs up.
// ponytail: polled, so `has_window` may count a just-closed window for up to this long; a hub unsubscribe would make it exact.
const CLOSED_POLL: Duration = Duration::from_millis(200);

/// accounts.toml is read-modified-written by one request at a time.
static CONFIG: Mutex<()> = Mutex::new(());
static TOKEN: AtomicU64 = AtomicU64::new(1);

/// One per client connection, with the client's own `Db`.
pub struct State {
    pub hub: Hub,
    pub paths: Paths,
    pub db: Db,
    /// Set by `subscribe`; the connection takes it and forwards the events.
    pub events: Option<Receiver<String>>,
    subscribed: bool,
}

impl State {
    pub fn new(hub: Hub, paths: Paths, db: Db) -> State {
        State { hub, paths, db, events: None, subscribed: false }
    }
}

/// Unknown fields are ignored (`id` among them). Commands of later tasks take their fields then.
#[derive(Deserialize)]
#[serde(tag = "cmd", rename_all = "snake_case")]
enum Req {
    Subscribe {
        #[serde(default)]
        window: bool,
    },
    Status,
    Accounts,
    AccountAdd(NewAccount),
    AccountRemove { email: String },
    AccountUpdate { email: String, name: String },
    Folders { account: Option<String> },
    List(ListQuery),
    Message { message: i64 },
    Search { q: String },
    Contacts { prefix: String },
    Sync { account: Option<String>, folder: Option<i64> },
    Attachment { message: i64, index: u32 },
    OpenRemote { account: String, folder_path: String, uid: u32 },
    Act { ids: Vec<i64>, op: Op },
    Undo { batch: i64 },
    Send(Draft),
    /// `outbox`: the positive id `send` returned (a negative one, as `list` shows it, works too).
    OutboxRetry { outbox: i64 },
    OutboxDiscard { outbox: i64 },
}

/// No `Debug`: it holds the password.
#[derive(Deserialize)]
struct NewAccount {
    provider: Provider,
    email: String,
    #[serde(default)]
    name: String,
    password: String,
    /// `imap` only; defaults to the email.
    login: Option<String>,
    /// `imap` only; gmail and yandex use the presets.
    imap: Option<Server>,
    smtp: Option<Server>,
}

fn reply(id: Value, r: Result<Value, String>) -> String {
    let mut out = json!({"id": id, "ok": r.is_ok()});
    match r {
        Ok(Value::Null) => {}
        Ok(v) => out["result"] = v,
        Err(e) => out["error"] = e.into(),
    }
    out.to_string()
}

fn val(v: impl Serialize) -> Result<Value, String> {
    serde_json::to_value(v).map_err(|e| e.to_string())
}

/// One request line in, one response line out (without the newline). Never panics.
pub fn dispatch(st: &mut State, line: &str) -> String {
    match serde_json::from_str::<Value>(line) {
        Ok(v) => {
            let id = v.get("id").cloned().unwrap_or(Value::Null);
            reply(id, serde_json::from_value(v).map_err(|e| e.to_string()).and_then(|req| handle(st, req)))
        }
        Err(e) => reply(Value::Null, Err(e.to_string())),
    }
}

fn handle(st: &mut State, req: Req) -> Result<Value, String> {
    match req {
        Req::Subscribe { window } => {
            if st.subscribed {
                return Err("already subscribed".into());
            }
            st.subscribed = true;
            st.events = Some(st.hub.subscribe(window));
            Ok(Value::Null)
        }
        Req::Status => {
            let accounts = st.hub.statuses().into_iter()
                .map(|a| Ok(json!({"email": a.email, "status": a.status, "queued": st.db.queued_count(&a.email)?})))
                .collect::<Result<Vec<_>, String>>()?;
            Ok(json!({"accounts": accounts}))
        }
        Req::Accounts => val(config::load(&st.paths.config)?),
        Req::AccountAdd(n) => account_add(st, n),
        Req::AccountRemove { email } => account_remove(st, &email),
        Req::AccountUpdate { email, name } => account_update(st, &email, &name),
        Req::Folders { account } => val(st.db.folder_list(account.as_deref())?),
        Req::List(q) => val(st.db.list(&q)?),
        Req::Message { message } => message_full(st, message),
        Req::Attachment { message, index } => attachment(st, message, index),
        Req::Search { q } => {
            let local = st.db.search(&q, SEARCH_LIMIT)?;
            let token = TOKEN.fetch_add(1, Relaxed);
            // Each mailbox answers with its own `search_remote` event.
            for h in st.hub.handles() {
                let _ = h.jobs.send(Job::Search { query: q.clone(), token });
            }
            Ok(json!({"local": local, "token": token}))
        }
        Req::Contacts { prefix } => val(st.db.contacts(&prefix, CONTACTS_LIMIT)?),
        Req::Sync { account, folder } => sync(st, account.as_deref(), folder),
        // A client bug: restore is what `undo` of a delete does.
        Req::Act { op: Op::Restore, .. } => Err("restore: use undo".into()),
        Req::Act { ids, op } => queued(st, actions::enqueue(&st.db, &ids, op)?),
        Req::Undo { batch } => queued(st, actions::undo(&st.db, batch)?),
        Req::OpenRemote { account, folder_path, uid } => {
            let id = ask(st, &account, |reply| Job::FetchRemote { folder_path, uid, reply })?;
            Ok(json!({"id": id}))
        }
        Req::OutboxRetry { outbox } => {
            st.db.outbox_retry(outbox.abs())?;
            st.hub.emit(Event::Changed { folders: false, ids: vec![] });
            smtp::wake();
            Ok(Value::Null)
        }
        Req::OutboxDiscard { outbox } => {
            st.db.outbox_discard(outbox.abs())?;
            st.hub.emit(Event::Changed { folders: true, ids: vec![] });
            Ok(Value::Null)
        }
        Req::Send(d) => {
            let acc = config::load(&st.paths.config)?.into_iter().find(|a| a.email == d.account).ok_or_else(no_account)?;
            let outbox = smtp::queue(&st.db, &acc, &d)?;
            st.hub.emit(Event::Changed { folders: false, ids: vec![] });
            smtp::wake();
            Ok(json!({"outbox": outbox}))
        }
    }
}

/// Hands a job to the account's worker and waits for its answer, never forever.
fn ask<T>(st: &State, account: &str, job: impl FnOnce(Sender<Result<T, String>>) -> Job) -> Result<T, String> {
    let h = st.hub.handles().into_iter().find(|h| h.email == account).ok_or_else(no_account)?;
    let (tx, rx) = channel();
    h.jobs.send(job(tx)).map_err(|_| format!("{account}: worker stopped"))?;
    match rx.recv_timeout(FETCH_WAIT) {
        Ok(r) => r,
        Err(RecvTimeoutError::Timeout) => Err(tr("no network: the server did not answer in time", "нет сети: сервер не ответил вовремя")),
        Err(RecvTimeoutError::Disconnected) => Err(format!("{account}: worker stopped")),
    }
}

/// Inline-image fetches per message (by cache dir): at most one per `CID_RETRY`.
fn retry_due(dir: &Path) -> bool {
    static LAST: Mutex<Option<HashMap<PathBuf, Instant>>> = Mutex::new(None);
    let mut g = lock(&LAST);
    let m = g.get_or_insert_with(HashMap::new);
    if m.get(dir).is_some_and(|t| t.elapsed() < fetch::CID_RETRY) {
        return false;
    }
    m.insert(dir.to_path_buf(), Instant::now());
    true
}

/// The message with its body (fetched if it is a big one) and `inline`: `cid` → cached image file.
fn message_full(st: &State, id: i64) -> Result<Value, String> {
    if id < 0 {
        let (full, error) = st.db.outbox_full(-id)?.ok_or_else(|| tr("message not found", "письмо не найдено"))?;
        let mut v = val(&full)?;
        v["inline"] = json!({});
        if let Some(e) = error {
            v["send_error"] = e.into();
        }
        return Ok(v);
    }
    let get = || st.db.message(id)?.ok_or_else(|| tr("message not found", "письмо не найдено"));
    let mut full = get()?;
    let dir = fetch::msg_dir(&st.paths.cache, id);
    if !full.has_body {
        if full.uid.is_none() {
            return Err(tr("message is being moved, try again", "письмо перемещается, попробуйте ещё раз"));
        }
        ask(st, &full.summary.account, |reply| Job::Fetch { message: id, reply })?;
        full = get()?;
    } else if full.uid.is_some() && fetch::cids_missing(&dir, &full) && retry_due(&dir) {
        // The text is shown now; the images come later with a `changed` event.
        if let Some(h) = st.hub.handles().into_iter().find(|h| h.email == full.summary.account) {
            let _ = h.jobs.send(Job::Fetch { message: id, reply: channel().0 });
        }
    }
    let mut v = val(&full)?;
    v["inline"] = val(fetch::inline_map(&dir, &full))?;
    Ok(v)
}

/// The file of an attachment: cached, or downloaded by the worker.
fn attachment(st: &State, id: i64, index: u32) -> Result<Value, String> {
    let full = st.db.message(id)?.ok_or_else(|| tr("message not found", "письмо не найдено"))?;
    let path = match fetch::cached_attachment(&fetch::msg_dir(&st.paths.cache, id), &full, index) {
        Some(p) => p,
        None if full.uid.is_none() => return Err(tr("message is being moved, try again", "письмо перемещается, попробуйте ещё раз")),
        None => ask(st, &full.summary.account, |reply| Job::Attachment { message: id, index, reply })?,
    };
    Ok(json!({"path": path}))
}

/// A batch of `act` or `undo` was applied: the workers start on it, the window reloads.
fn queued(st: &State, batch: i64) -> Result<Value, String> {
    actions::kick(&st.db, &st.hub, batch);
    Ok(json!({"batch": batch}))
}

fn no_account() -> String {
    tr("no such account", "нет такого ящика")
}

/// Checked fields → the account to store; nothing touches the network yet.
fn new_account(n: &NewAccount) -> Result<Account, String> {
    let email = n.email.trim();
    if !email.contains('@') || email.contains(char::is_whitespace) {
        return Err(tr("invalid email address", "неверный адрес почты"));
    }
    if n.password.is_empty() {
        return Err(tr("empty password", "пустой пароль"));
    }
    let ok = |s: &Option<Server>| s.as_ref().filter(|s| !s.host.trim().is_empty() && s.port != 0).cloned();
    let (imap, smtp, append_sent, login) = match (config::preset(n.provider), ok(&n.imap), ok(&n.smtp)) {
        (Some((i, s, a)), _, _) => (i, s, a, email.to_string()),
        // Most servers do not keep a copy of what SMTP sent, so it is appended to Sent.
        (None, Some(i), Some(s)) => (i, s, true, n.login.clone().filter(|l| !l.trim().is_empty()).unwrap_or_else(|| email.into())),
        _ => return Err(tr("IMAP and SMTP servers are required", "нужны серверы IMAP и SMTP")),
    };
    Ok(Account { email: email.into(), name: n.name.trim().into(), provider: n.provider, login, imap, smtp, append_sent })
}

/// Trial login first: a wrong password never reaches the keyring or accounts.toml.
fn account_add(st: &State, n: NewAccount) -> Result<Value, String> {
    let acc = new_account(&n)?;
    let _one = lock(&CONFIG);
    let mut all = config::load(&st.paths.config)?;
    if all.iter().any(|a| a.email.eq_ignore_ascii_case(&acc.email)) {
        return Err(tr("this mailbox is already added", "этот ящик уже добавлен"));
    }
    drop(Conn::connect(&acc, &n.password)?);
    secret::store(&acc.email, &n.password)?;
    all.push(acc.clone());
    config::save(&st.paths.config, &all)?;
    st.hub.add_handle(account::spawn(acc.clone(), st.paths.clone(), st.hub.clone()));
    val(acc)
}

/// Threads first (stopped and joined), then the rows, the password and the config entry.
fn account_remove(st: &State, email: &str) -> Result<Value, String> {
    let _one = lock(&CONFIG);
    let mut all = config::load(&st.paths.config)?;
    let i = all.iter().position(|a| a.email == email).ok_or_else(no_account)?;
    st.hub.remove_handle(email);
    st.db.remove_account(email)?;
    secret::clear(email)?;
    all.remove(i);
    config::save(&st.paths.config, &all)?;
    st.hub.emit(Event::Changed { folders: true, ids: vec![] });
    Ok(Value::Null)
}

/// Only accounts.toml changes: `send` reads the account from it per mail, so the next "From" has
/// the new name; no thread keeps the name.
fn account_update(st: &State, email: &str, name: &str) -> Result<Value, String> {
    let _one = lock(&CONFIG);
    let mut all = config::load(&st.paths.config)?;
    all.iter_mut().find(|a| a.email == email).ok_or_else(no_account)?.name = name.trim().into();
    config::save(&st.paths.config, &all)?;
    st.hub.emit_status();
    Ok(Value::Null)
}

/// A folder → `SyncFolder` to its account; otherwise "sync now" (`retry`) for the account or all.
fn sync(st: &State, account: Option<&str>, folder: Option<i64>) -> Result<Value, String> {
    let mut hs: Vec<Handle> = st.hub.handles();
    if let Some(a) = account {
        hs.retain(|h| h.email == a);
        if hs.is_empty() {
            return Err(no_account());
        }
    }
    let Some(f) = folder else {
        hs.iter().for_each(Handle::retry);
        return Ok(Value::Null);
    };
    let owner = hs.iter().find(|h| st.db.folders_of(&h.email).is_ok_and(|v| v.iter().any(|x| x.id == f)));
    let h = owner.ok_or_else(|| tr("no such folder", "нет такой папки"))?;
    h.jobs.send(Job::SyncFolder(f)).map_err(|_| format!("{}: worker stopped", h.email))?;
    Ok(Value::Null)
}

/// Refuses to start next to a running maild; a stale socket file is replaced. Mode 0600.
pub fn listen(path: &Path) -> Result<UnixListener, String> {
    let err = |e: std::io::Error| format!("{}: {e}", path.display());
    if UnixStream::connect(path).is_ok() {
        return Err(format!("{}: maild is already running", path.display()));
    }
    match std::fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(err(e)),
        _ => {}
    }
    let l = UnixListener::bind(path).map_err(err)?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600)).map_err(err)?;
    Ok(l)
}

/// Never returns.
pub fn serve(listener: UnixListener, paths: Paths, hub: Hub) {
    for conn in listener.incoming() {
        match conn {
            Ok(s) => {
                let (paths, hub) = (paths.clone(), hub.clone());
                std::thread::spawn(move || client(s, paths, hub));
            }
            Err(e) => {
                eprintln!("socket: {e}");
                std::thread::sleep(Duration::from_millis(100)); // e.g. out of fds: do not spin
            }
        }
    }
}

/// A failed write (client gone or not reading) shuts the connection, so the client sees it.
fn send(out: &Mutex<UnixStream>, line: &str) -> Result<(), ()> {
    let mut s = lock(out);
    s.write_all(format!("{line}\n").as_bytes()).map_err(|_| {
        let _ = s.shutdown(Shutdown::Both);
    })
}

/// Requests one at a time, in order; events (if subscribed) from a second thread.
fn client(stream: UnixStream, paths: Paths, hub: Hub) {
    let db = match Db::open(&paths.db) {
        Ok(d) => d,
        Err(e) => return eprintln!("socket client: {e}"),
    };
    let Ok(out) = stream.try_clone() else { return };
    let _ = out.set_write_timeout(Some(WRITE_TIMEOUT));
    let out = Arc::new(Mutex::new(out));
    let closed = Arc::new(AtomicBool::new(false));
    let mut st = State::new(hub, paths, db);
    let mut reader = BufReader::new(stream);
    let mut buf = Vec::new();
    loop {
        buf.clear();
        match (&mut reader).take(MAX_LINE as u64 + 1).read_until(b'\n', &mut buf) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        if buf.len() > MAX_LINE && buf.last() != Some(&b'\n') {
            let _ = send(&out, &reply(Value::Null, Err(format!("request longer than {MAX_LINE} bytes"))));
            break;
        }
        let resp = match std::str::from_utf8(&buf) {
            Ok(line) if line.trim().is_empty() => continue,
            Ok(line) => dispatch(&mut st, line),
            Err(e) => reply(Value::Null, Err(e.to_string())),
        };
        if send(&out, &resp).is_err() {
            break;
        }
        if let Some(rx) = st.events.take() {
            let (out, closed) = (out.clone(), closed.clone());
            std::thread::spawn(move || forward(&rx, &out, &closed));
        }
    }
    closed.store(true, Relaxed);
}

/// Until the client hangs up or stops reading. The hub's empty probes are not written.
fn forward(rx: &Receiver<String>, out: &Mutex<UnixStream>, closed: &AtomicBool) {
    while !closed.load(Relaxed) {
        match rx.recv_timeout(CLOSED_POLL) {
            Ok(line) if line.is_empty() => {}
            Ok(line) if send(out, &line).is_err() => return,
            Ok(_) | Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => return,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::account::Status;
    use crate::config::{self, Account, Provider};
    use crate::db::Role;
    use crate::mime::{Addr, Parsed};
    use crate::hub::Event;
    use serde_json::{Value, json};
    use std::io::{BufRead, BufReader, Write};
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixStream;
    use std::time::{Duration, Instant};

    fn paths(name: &str) -> Paths {
        let d = std::env::temp_dir().join(format!("maild-test-{}-ipc-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        Paths { config: d.join("accounts.toml"), db: d.join("mail.db"), cache: d.join("cache"), socket: d.join("s.sock"), notify_ids: d.join("notify-ids.json") }
    }

    fn state(name: &str) -> State {
        State::new(Hub::default(), paths(name), Db::memory())
    }

    fn call(st: &mut State, req: Value) -> Value {
        serde_json::from_str(&dispatch(st, &req.to_string())).unwrap()
    }

    fn msg(subject: &str, date: i64) -> Parsed {
        Parsed { subject: subject.into(), date, from: Addr { name: "A".into(), addr: "a@x.org".into() }, ..Default::default() }
    }

    #[test]
    fn unknown_cmd_error() {
        let r = call(&mut state("unknown"), json!({"id": 7, "cmd": "frobnicate"}));
        assert_eq!((r["id"].clone(), r["ok"].clone()), (json!(7), json!(false)));
        assert!(r["error"].as_str().unwrap().contains("frobnicate"), "{r}");
    }

    #[test]
    fn bad_json_error() {
        let mut st = state("badjson");
        for line in ["{not json", "", "[1,2]", "{\"id\":1}", "{\"id\":2,\"cmd\":\"list\",\"limit\":\"x\"}"] {
            let r: Value = serde_json::from_str(&dispatch(&mut st, line)).unwrap();
            assert_eq!(r["ok"], false, "{line}: {r}");
            assert!(r["error"].is_string(), "{line}: {r}");
        }
    }

    #[test]
    fn list_returns_summaries() {
        let mut st = state("list");
        let f = st.db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        st.db.insert_message(f, 1, false, false, 1, &msg("older", 5), true).unwrap();
        st.db.insert_message(f, 2, true, false, 1, &msg("newer", 9), true).unwrap();
        let r = call(&mut st, json!({"id": 1, "cmd": "list", "role": "inbox", "limit": 10}));
        assert_eq!(r["ok"], true, "{r}");
        let subjects: Vec<&str> = r["result"].as_array().unwrap().iter().map(|s| s["subject"].as_str().unwrap()).collect();
        assert_eq!(subjects, ["newer", "older"]);
        let r = call(&mut st, json!({"id": 2, "cmd": "list", "folder": f, "unread_only": true}));
        assert_eq!(r["result"].as_array().unwrap().len(), 1);
        assert_eq!(r["result"][0]["seen"], false);
    }

    #[test]
    fn folders_unified() {
        let mut st = state("folders");
        let f = st.db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        st.db.insert_message(f, 1, false, false, 1, &msg("s", 5), true).unwrap();
        let r = call(&mut st, json!({"id": 1, "cmd": "folders"}));
        let roles: Vec<&str> = r["result"].as_array().unwrap().iter().map(|f| f["role"].as_str().unwrap()).collect();
        assert_eq!(roles, ["inbox", "flagged", "sent", "trash", "junk"]);
        assert_eq!((r["result"][0]["unread"].clone(), r["result"][0]["id"].clone()), (json!(1), Value::Null));
        let r = call(&mut st, json!({"id": 2, "cmd": "folders", "account": "a@x"}));
        assert_eq!(r["result"][0]["id"], f);
    }

    #[test]
    fn subscribe_window_sets_has_window() {
        let mut st = state("sub");
        assert!(!st.hub.has_window());
        let r = call(&mut st, json!({"id": 1, "cmd": "subscribe", "window": true}));
        assert_eq!(r, json!({"id": 1, "ok": true}));
        assert!(st.hub.has_window());
        assert!(st.events.is_some());
        assert_eq!(call(&mut st, json!({"id": 2, "cmd": "subscribe"}))["ok"], false, "second subscription");
        drop(st.events.take());
        assert!(!st.hub.has_window());
    }

    #[test]
    fn subscribe_without_window() {
        let mut st = state("subctl");
        assert_eq!(call(&mut st, json!({"cmd": "subscribe"})), json!({"id": null, "ok": true}));
        assert!(!st.hub.has_window());
    }

    #[test]
    fn status_with_queue() {
        let mut st = state("status");
        st.hub.set_status("a@x", Status::Online);
        let r = call(&mut st, json!({"id": 1, "cmd": "status"}));
        assert_eq!(r["result"], json!({"accounts": [{"email": "a@x", "status": "online", "queued": 0}]}));
    }

    #[test]
    fn message_full_or_error() {
        let mut st = state("message");
        let f = st.db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let id = st.db.insert_message(f, 1, false, false, 1, &msg("hello", 5), true).unwrap();
        let r = call(&mut st, json!({"id": 1, "cmd": "message", "message": id}));
        assert_eq!(r["result"]["summary"]["subject"], "hello", "{r}");
        assert_eq!(call(&mut st, json!({"id": 2, "cmd": "message", "message": id + 1}))["ok"], false);
    }

    #[test]
    fn message_and_attachment_without_worker() {
        let mut st = state("ondemand");
        let f = st.db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        // a plain-text message: no fetch, empty `inline`
        let id = st.db.insert_message(f, 1, false, false, 1, &msg("hello", 5), true).unwrap();
        assert_eq!(call(&mut st, json!({"id": 1, "cmd": "message", "message": id}))["result"]["inline"], json!({}));
        // a big one without body, no account running: an error, not a hang
        let big = st.db.insert_message(f, 2, false, false, 1, &msg("big", 5), false).unwrap();
        assert_eq!(call(&mut st, json!({"id": 2, "cmd": "message", "message": big}))["ok"], false);
        // its move is pending: no guessing
        st.db.c.execute("UPDATE messages SET uid = NULL WHERE id = ?1", [big]).unwrap();
        let r = call(&mut st, json!({"id": 3, "cmd": "message", "message": big}));
        assert_eq!(r["error"], tr("message is being moved, try again", "письмо перемещается, попробуйте ещё раз"), "{r}");
        // an attachment already in the cache is returned without any worker
        let at = crate::mime::Attachment { index: 0, name: "../x.txt".into(), mime: "text/plain".into(), size: 1, cid: None, inline: false };
        let mut p = msg("att", 5);
        p.attachments = vec![at];
        let a = st.db.insert_message(f, 3, false, false, 1, &p, true).unwrap();
        let dir = st.paths.cache.join(a.to_string());
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::create_dir_all(dir.join("0")).unwrap();
        std::fs::write(dir.join("0/x.txt"), "t").unwrap();
        let r = call(&mut st, json!({"id": 4, "cmd": "attachment", "message": a, "index": 0}));
        assert_eq!(r["result"]["path"], dir.join("0/x.txt").to_str().unwrap(), "{r}");
        assert_eq!(call(&mut st, json!({"id": 5, "cmd": "attachment", "message": a, "index": 7}))["ok"], false, "not cached, no account");
    }

    #[test]
    fn missing_cid_does_not_block_or_refetch() {
        use std::sync::atomic::AtomicUsize;
        let mut st = state("cidretry");
        st.db = Db::open(&st.paths.db).unwrap();
        let f = st.db.upsert_folder("me@x", "INBOX", Role::Inbox).unwrap();
        let mut p = msg("cid", 5);
        p.html = Some("<img src=\"cid:gone@x\">".into());
        p.attachments = vec![crate::mime::Attachment { index: 0, name: "l".into(), mime: "image/png".into(), size: 1, cid: Some("gone@x".into()), inline: true }];
        let id = st.db.insert_message(f, 5, false, false, 1, &p, true).unwrap();
        let fetches = Arc::new(AtomicUsize::new(0));
        let n = fetches.clone();
        let raw = "From: a@x\r\n\r\nplain";
        let addr = crate::imap::fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "LIST \"\" *" => "* LIST () \"/\" \"INBOX\"\r\n".into(),
            "EXAMINE \"INBOX\"" => "* 1 EXISTS\r\n".into(),
            "UID SEARCH SINCE 1-Jan-2026" => "* SEARCH 5\r\n".into(),
            "UID FETCH 5:* (UID FLAGS)" => "* 1 FETCH (UID 5 FLAGS ())\r\n".into(),
            "UID FETCH 5 (UID BODY.PEEK[])" => { n.fetch_add(1, Relaxed); format!("* 1 FETCH (UID 5 BODY[] {{{}}}\r\n{raw})\r\n", raw.len()) }
            "SELECT \"INBOX\"" | "IDLE" => String::new(),
            _ => return None,
        }));
        let s = Server { host: "h".into(), port: 1 };
        let acc = Account { email: "me@x".into(), name: "N".into(), provider: Provider::Imap, login: "me".into(), imap: s.clone(), smtp: s, append_sent: false };
        let connect = move || Conn::plain(std::net::TcpStream::connect(addr).map_err(|e| e.to_string())?, "me", "pw");
        let h = account::spawn_with(acc, st.paths.db.clone(), st.paths.cache.clone(), st.hub.clone(), Arc::new(connect));
        st.hub.add_handle(h.clone());

        let t = Instant::now();
        let r = call(&mut st, json!({"id": 1, "cmd": "message", "message": id}));
        assert!(t.elapsed() < Duration::from_millis(250) && r["result"]["inline"] == json!({}), "{r}");
        let end = Instant::now() + Duration::from_secs(2);
        while fetches.load(Relaxed) == 0 { assert!(Instant::now() < end, "no background fetch"); std::thread::sleep(Duration::from_millis(5)); }
        std::thread::sleep(Duration::from_millis(100));
        call(&mut st, json!({"id": 2, "cmd": "message", "message": id}));
        std::thread::sleep(Duration::from_millis(200));
        assert_eq!(fetches.load(Relaxed), 1, "refetched within the backoff");
        h.stop_and_join();
    }

    #[test]
    fn search_local_and_token() {
        let mut st = state("search");
        let f = st.db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        st.db.insert_message(f, 1, false, false, 1, &msg("договор поставки", 5), true).unwrap();
        let a = call(&mut st, json!({"id": 1, "cmd": "search", "q": "догов"}));
        assert_eq!(a["result"]["local"].as_array().unwrap().len(), 1, "{a}");
        let b = call(&mut st, json!({"id": 2, "cmd": "search", "q": "nothing"}));
        assert!(b["result"]["local"].as_array().unwrap().is_empty());
        assert!(b["result"]["token"].as_u64().unwrap() > a["result"]["token"].as_u64().unwrap());
    }

    #[test]
    fn contacts_from_sent() {
        let mut st = state("contacts");
        let f = st.db.upsert_folder("a@x", "Sent", Role::Sent).unwrap();
        let mut m = msg("s", 5);
        m.to = vec![Addr { name: "Bob".into(), addr: "bob@y.org".into() }];
        st.db.insert_message(f, 1, true, false, 1, &m, true).unwrap();
        let r = call(&mut st, json!({"id": 1, "cmd": "contacts", "prefix": "bo"}));
        assert_eq!(r["result"], json!([{"name": "Bob", "addr": "bob@y.org"}]));
    }

    #[test]
    fn act_and_undo() {
        let mut st = state("act");
        let f = st.db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let id = st.db.insert_message(f, 1, false, false, 1, &msg("s", 5), true).unwrap();
        let events = st.hub.subscribe(false);
        let r = call(&mut st, json!({"id": 1, "cmd": "act", "ids": [id], "op": "read"}));
        assert_eq!(r, json!({"id": 1, "ok": true, "result": {"batch": 1}}));
        assert_eq!(events.try_recv().unwrap(), format!(r#"{{"event":"changed","folders":true,"ids":[{id}]}}"#));
        assert_eq!(call(&mut st, json!({"id": 2, "cmd": "act", "ids": [id], "op": "read"}))["result"], json!({"batch": 0}), "already read");
        assert_eq!(call(&mut st, json!({"id": 3, "cmd": "undo", "batch": 1}))["result"], json!({"batch": 2}));
        assert!(!st.db.message(id).unwrap().unwrap().summary.seen);
        for bad in [json!({"cmd": "act", "ids": [id], "op": "restore"}), json!({"cmd": "act", "ids": [-1], "op": "read"}),
                    json!({"cmd": "act", "ids": [id], "op": "delete"}), json!({"cmd": "undo", "batch": 0})] {
            assert_eq!(call(&mut st, bad.clone())["ok"], false, "{bad}");
        }
    }

    #[test]
    fn send_queues_and_validates() {
        let mut st = state("send");
        let (imap, smtp, append_sent) = (Server { host: "h".into(), port: 1 }, Server { host: "h".into(), port: 465 }, false);
        let acc = Account { email: "a@x".into(), name: String::new(), provider: Provider::Imap, login: "a".into(), imap, smtp, append_sent };
        config::save(&st.paths.config, &[acc]).unwrap();
        let events = st.hub.subscribe(false);
        let draft = |account: &str, to: Value| json!({"id": 1, "cmd": "send", "account": account, "to": to, "subject": "s", "text": "t"});
        let r = call(&mut st, draft("a@x", json!([{"name": "", "addr": "b@y.org"}])));
        assert_eq!(r["ok"], true, "{r}");
        assert!(r["result"]["outbox"].as_i64().unwrap() > 0);
        assert_eq!(st.db.queued_count("a@x").unwrap(), 1);
        assert_eq!(events.try_recv().unwrap(), r#"{"event":"changed","folders":false,"ids":[]}"#);
        assert_eq!(call(&mut st, draft("no@x", json!([{"name": "", "addr": "b@y.org"}])))["ok"], false);
        assert_eq!(call(&mut st, draft("a@x", json!([])))["ok"], false);
        assert_eq!(st.db.queued_count("a@x").unwrap(), 1);
    }

    #[test]
    fn outbox_retry_discard_and_message() {
        let mut st = state("outbox");
        let acc = Account { email: "a@x".into(), name: "A".into(), provider: Provider::Imap, login: "a".into(), imap: Server { host: "h".into(), port: 1 }, smtp: Server { host: "h".into(), port: 465 }, append_sent: false };
        config::save(&st.paths.config, &[acc]).unwrap();
        let send = |st: &mut State, subject: &str| {
            let r = call(st, json!({"id": 1, "cmd": "send", "account": "a@x", "to": [{"name": "", "addr": "b@y.org"}], "subject": subject, "text": "body text"}));
            r["result"]["outbox"].as_i64().unwrap()
        };
        let (one, two) = (send(&mut st, "one"), send(&mut st, "two"));
        // the negative id shows the mail
        let r = call(&mut st, json!({"id": 2, "cmd": "message", "message": -one}));
        assert_eq!(r["ok"], true, "{r}");
        assert_eq!((r["result"]["summary"]["subject"].clone(), r["result"]["summary"]["queued"].clone(), r["result"]["summary"]["id"].clone()), (json!("one"), json!(true), json!(-one)), "{r}");
        assert_eq!(r["result"]["text"].as_str().unwrap().trim(), "body text");
        assert_eq!(call(&mut st, json!({"id": 3, "cmd": "message", "message": -999}))["ok"], false);
        // retry: only a failed row
        assert_eq!(call(&mut st, json!({"id": 4, "cmd": "outbox_retry", "outbox": one}))["ok"], false);
        st.db.outbox_fail(one, "550 no").unwrap();
        let r = call(&mut st, json!({"id": 5, "cmd": "message", "message": -one}));
        assert_eq!((r["result"]["summary"]["failed"].clone(), r["result"]["send_error"].clone()), (json!(true), json!("550 no")), "{r}");
        assert_eq!(call(&mut st, json!({"id": 6, "cmd": "outbox_retry", "outbox": one}))["ok"], true);
        assert_eq!(st.db.queued_count("a@x").unwrap(), 2);
        // discard: queued or failed
        st.db.outbox_fail(two, "x").unwrap();
        assert_eq!(call(&mut st, json!({"id": 7, "cmd": "outbox_discard", "outbox": two}))["ok"], true);
        assert_eq!(call(&mut st, json!({"id": 8, "cmd": "outbox_discard", "outbox": -one}))["ok"], true);
        assert_eq!(call(&mut st, json!({"id": 9, "cmd": "outbox_discard", "outbox": one}))["ok"], false);
        assert_eq!(st.db.queued_count("a@x").unwrap(), 0);
        // a sent row is not discarded
        let three = send(&mut st, "three");
        st.db.outbox_sent(three).unwrap();
        assert_eq!(call(&mut st, json!({"id": 10, "cmd": "outbox_discard", "outbox": three}))["ok"], false);
    }

    #[test]
    fn open_remote_unknown_account() {
        let mut st = state("open-remote");
        let r = call(&mut st, json!({"id": 1, "cmd": "open_remote", "account": "no@x", "folder_path": "INBOX", "uid": 3}));
        assert_eq!(r["ok"], false);
    }

    #[test]
    fn sync_unknown_account() {
        let mut st = state("sync");
        assert_eq!(call(&mut st, json!({"id": 1, "cmd": "sync"}))["ok"], true);
        assert_eq!(call(&mut st, json!({"id": 2, "cmd": "sync", "account": "nobody@x"}))["ok"], false);
        assert_eq!(call(&mut st, json!({"id": 3, "cmd": "sync", "folder": 99}))["ok"], false);
    }

    #[test]
    fn accounts_from_config() {
        let mut st = state("accounts");
        let (imap, smtp, append_sent) = config::preset(Provider::Gmail).unwrap();
        let a = Account { email: "a@x".into(), name: "N".into(), provider: Provider::Gmail, login: "a@x".into(), imap, smtp, append_sent };
        config::save(&st.paths.config, &[a]).unwrap();
        let r = call(&mut st, json!({"id": 1, "cmd": "accounts"}));
        assert_eq!(r["result"][0]["email"], "a@x");
        assert_eq!(r["result"][0]["imap"], json!({"host": "imap.gmail.com", "port": 993}));
        // validation fails before any login, keyring or network
        let add = |st: &mut State, extra: Value| {
            let mut req = json!({"id": 1, "cmd": "account_add", "provider": "imap", "email": "b@x", "password": "pw"});
            req.as_object_mut().unwrap().extend(extra.as_object().unwrap().clone());
            call(st, req)
        };
        assert_eq!(add(&mut st, json!({"provider": "gmail", "email": "A@X"}))["ok"], false, "duplicate");
        assert_eq!(add(&mut st, json!({}))["ok"], false, "imap without servers");
        assert_eq!(add(&mut st, json!({"imap": {"host": "", "port": 993}, "smtp": {"host": "s", "port": 465}}))["ok"], false);
        assert_eq!(add(&mut st, json!({"provider": "gmail", "email": "no-at"}))["ok"], false);
        assert_eq!(add(&mut st, json!({"provider": "gmail", "password": ""}))["ok"], false);
        assert_eq!(add(&mut st, json!({"provider": "pop3"}))["ok"], false);
        assert_eq!(call(&mut st, json!({"id": 1, "cmd": "account_remove", "email": "b@x"}))["ok"], false);
        assert_eq!(config::load(&st.paths.config).unwrap().len(), 1);
        let _ = std::fs::remove_dir_all(st.paths.config.parent().unwrap());
    }

    #[test]
    fn account_update_renames() {
        let mut st = state("rename");
        let (imap, smtp, append_sent) = config::preset(Provider::Gmail).unwrap();
        let a = Account { email: "a@x.org".into(), name: "Old".into(), provider: Provider::Gmail, login: "a@x.org".into(), imap, smtp, append_sent };
        config::save(&st.paths.config, &[a.clone(), Account { email: "b@x.org".into(), ..a }]).unwrap();
        let events = st.hub.subscribe(false);
        let r = call(&mut st, json!({"id": 1, "cmd": "account_update", "email": "a@x.org", "name": " Иван Петров "}));
        assert_eq!(r, json!({"id": 1, "ok": true}));
        let all = config::load(&st.paths.config).unwrap();
        assert_eq!((all[0].name.as_str(), all[1].name.as_str()), ("Иван Петров", "Old"));
        assert!(events.try_recv().unwrap().starts_with(r#"{"event":"status""#));
        // the next mail goes out with the new From, nothing restarted
        let s = call(&mut st, json!({"id": 2, "cmd": "send", "account": "a@x.org", "to": [{"name": "", "addr": "b@y.org"}], "subject": "s", "text": "t"}));
        assert_eq!(s["ok"], true, "{s}");
        let (_, _, _, raw) = st.db.outbox_queued().unwrap().pop().unwrap();
        let from = crate::mime::parse(&raw).from;
        assert_eq!((from.name.as_str(), from.addr.as_str()), ("Иван Петров", "a@x.org"));
        let _ = std::fs::remove_dir_all(st.paths.config.parent().unwrap());
    }

    #[test]
    fn account_update_unknown_err() {
        let mut st = state("rename-unknown");
        assert_eq!(call(&mut st, json!({"id": 1, "cmd": "account_update", "email": "no@x", "name": "N"}))["ok"], false);
        assert!(!st.paths.config.exists(), "no accounts.toml written for an unknown mailbox");
    }

    fn read_line(r: &mut BufReader<UnixStream>) -> String {
        let mut s = String::new();
        r.read_line(&mut s).unwrap();
        s
    }

    #[test]
    fn socket_roundtrip() {
        let p = paths("sock");
        std::fs::create_dir_all(p.socket.parent().unwrap()).unwrap();
        std::fs::write(&p.socket, "stale").unwrap();
        let l = listen(&p.socket).unwrap();
        assert_eq!(std::fs::metadata(&p.socket).unwrap().permissions().mode() & 0o777, 0o600);
        assert!(listen(&p.socket).is_err(), "a second instance must not take the socket");
        let hub = Hub::default();
        std::thread::spawn({
            let (p, hub) = (p.clone(), hub.clone());
            move || serve(l, p, hub)
        });

        let mut w = UnixStream::connect(&p.socket).unwrap();
        let mut r = BufReader::new(w.try_clone().unwrap());
        w.write_all(b"{\"id\":1,\"cmd\":\"subscribe\",\"window\":true}\n").unwrap();
        assert_eq!(read_line(&mut r), "{\"id\":1,\"ok\":true}\n");
        assert!(hub.has_window()); // its empty probe must not reach the socket
        hub.emit(Event::Open { id: 5 });
        assert_eq!(read_line(&mut r), "{\"event\":\"open\",\"id\":5}\n");
        w.write_all(b"\n{\"id\":2,\"cmd\":\"folders\"}\n").unwrap(); // blank lines are skipped
        let v: Value = serde_json::from_str(&read_line(&mut r)).unwrap();
        assert_eq!((v["id"].clone(), v["ok"].clone()), (json!(2), json!(true)), "{v}");
        drop((w, r));
        let end = Instant::now() + Duration::from_secs(2);
        while hub.has_window() {
            assert!(Instant::now() < end, "subscription outlived its client");
            std::thread::sleep(Duration::from_millis(10));
        }

        let mut w = UnixStream::connect(&p.socket).unwrap();
        let mut r = BufReader::new(w.try_clone().unwrap());
        w.write_all(&vec![b'x'; MAX_LINE + 1]).unwrap();
        let v: Value = serde_json::from_str(&read_line(&mut r)).unwrap();
        assert_eq!(v["ok"], false);
        assert_eq!(read_line(&mut r), "", "client not dropped after an over-long line");
        let _ = std::fs::remove_dir_all(p.socket.parent().unwrap());
    }
}
