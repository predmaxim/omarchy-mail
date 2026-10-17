//! SQLite store: folders, messages (with FTS5 search), pending actions, outbox.
//! Every thread opens its own `Db` on the same file (WAL + busy_timeout).

use crate::mime::{self, Addr, Attachment, Parsed};
use rusqlite::functions::FunctionFlags;
use rusqlite::{Connection, Row, params, params_from_iter, types::Value};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Role { Inbox, Sent, Trash, Junk, Flagged, Other, Remote }

impl Role {
    pub fn as_str(self) -> &'static str {
        match self {
            Role::Inbox => "inbox", Role::Sent => "sent", Role::Trash => "trash", Role::Junk => "junk",
            Role::Flagged => "flagged", Role::Other => "other", Role::Remote => "remote",
        }
    }
    fn from_db(s: &str) -> Role {
        match s {
            "inbox" => Role::Inbox, "sent" => Role::Sent, "trash" => Role::Trash, "junk" => Role::Junk,
            "flagged" => Role::Flagged, "remote" => Role::Remote, _ => Role::Other,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Folder { pub id: i64, pub account: String, pub path: String, pub role: Role, pub uidvalidity: u32, pub last_uid: u32, pub notified_uid: u32 }

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Summary {
    pub id: i64, pub account: String, pub role: Role, pub folder_path: String, pub date: i64,
    pub from: Addr, pub subject: String, pub preview: String, pub seen: bool, pub flagged: bool,
    pub has_attachments: bool, pub queued: bool,
    /// An outbox row the server refused for good (`queued` stays true).
    #[serde(default)]
    pub failed: bool,
    /// Server search hits only (`id` 0): what `open_remote` takes.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub remote: Option<Remote>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Remote { pub account: String, pub folder_path: String, pub uid: u32 }

/// Also the `list` request: absent fields take the defaults.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct ListQuery { pub account: Option<String>, pub role: Option<Role>, pub folder: Option<i64>, pub unread_only: bool, pub offset: u32, pub limit: u32 }

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FolderInfo {
    pub account: Option<String>, pub id: Option<i64>, pub path: String,
    /// Readable: last path component, modified UTF-7 decoded; the role name for unified entries.
    pub name: String, pub role: Role, pub unread: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Full {
    pub summary: Summary, pub to: Vec<Addr>, pub text: String, pub html: Option<String>,
    pub attachments: Vec<Attachment>, pub has_body: bool, pub message_id: String, pub in_reply_to: String,
    /// Space-separated message ids without angle brackets.
    pub refs: String, pub folder: i64, pub uid: Option<u32>,
}

/// Last path component, modified UTF-7 decoded.
// ponytail: splits on the common delimiters '/' and '|'; '.'-delimited servers show the whole path. Store LIST's delimiter if that matters.
fn display_name(path: &str) -> String {
    crate::imap::decode_mutf7(path.rsplit(['/', '|']).next().unwrap_or(path))
}

/// `c` is open to the crate for modules that keep their own tables (`actions`).
pub struct Db { pub(crate) c: Connection }

fn e(err: rusqlite::Error) -> String { err.to_string() }

/// Version 1. Message and outbox ids are never reused (AUTOINCREMENT): the cache, notifications
/// and the window refer to mail by id.
const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS folders(
  id INTEGER PRIMARY KEY, account TEXT NOT NULL, path TEXT NOT NULL, role TEXT NOT NULL,
  uidvalidity INTEGER, last_uid INTEGER NOT NULL DEFAULT 0, notified_uid INTEGER NOT NULL DEFAULT 0,
  UNIQUE(account, path));
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY AUTOINCREMENT, folder INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE, uid INTEGER,
  message_id TEXT NOT NULL DEFAULT '', in_reply_to TEXT NOT NULL DEFAULT '', refs TEXT NOT NULL DEFAULT '',
  date INTEGER NOT NULL DEFAULT 0, from_name TEXT NOT NULL DEFAULT '', from_addr TEXT NOT NULL DEFAULT '',
  to_json TEXT NOT NULL DEFAULT '[]', subject TEXT NOT NULL DEFAULT '', preview TEXT NOT NULL DEFAULT '',
  text TEXT NOT NULL DEFAULT '', html TEXT, has_body INTEGER NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0,
  seen INTEGER NOT NULL DEFAULT 0, flagged INTEGER NOT NULL DEFAULT 0, attachments_json TEXT NOT NULL DEFAULT '[]',
  UNIQUE(folder, uid));
CREATE INDEX IF NOT EXISTS messages_date ON messages(date DESC);
CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
  subject, from_text, to_text, body, content='messages', tokenize='unicode61 remove_diacritics 2');
CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
  INSERT INTO messages_fts(rowid, subject, from_text, to_text, body) VALUES (new.id, new.subject,
    new.from_name || ' ' || new.from_addr, mail_addrs(new.to_json), CASE WHEN new.text <> '' THEN new.text ELSE mail_strip(new.html) END);
END;
CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
  INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, to_text, body) VALUES ('delete', old.id, old.subject,
    old.from_name || ' ' || old.from_addr, mail_addrs(old.to_json), CASE WHEN old.text <> '' THEN old.text ELSE mail_strip(old.html) END);
END;
CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE OF subject, from_name, from_addr, to_json, text, html ON messages BEGIN
  INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, to_text, body) VALUES ('delete', old.id, old.subject,
    old.from_name || ' ' || old.from_addr, mail_addrs(old.to_json), CASE WHEN old.text <> '' THEN old.text ELSE mail_strip(old.html) END);
  INSERT INTO messages_fts(rowid, subject, from_text, to_text, body) VALUES (new.id, new.subject,
    new.from_name || ' ' || new.from_addr, mail_addrs(new.to_json), CASE WHEN new.text <> '' THEN new.text ELSE mail_strip(new.html) END);
END;
CREATE TABLE IF NOT EXISTS actions(
  id INTEGER PRIMARY KEY, batch INTEGER, account TEXT, op TEXT, message INTEGER, from_folder INTEGER,
  to_folder INTEGER, uid INTEGER, uidvalidity INTEGER, message_id TEXT, state TEXT, error TEXT,
  tries INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS actions_message ON actions(message);
CREATE TABLE IF NOT EXISTS outbox(
  id INTEGER PRIMARY KEY AUTOINCREMENT, account TEXT NOT NULL, raw BLOB, to_json TEXT NOT NULL DEFAULT '[]',
  subject TEXT NOT NULL DEFAULT '', date INTEGER NOT NULL DEFAULT 0, state TEXT, error TEXT);
";

const SUMMARY_COLS: &str = "m.id, f.account, f.role, f.path, m.date, m.from_name, m.from_addr, m.subject, m.preview, m.seen, m.flagged, m.attachments_json";
const SUMMARY_FROM: &str = "FROM messages m JOIN folders f ON f.id = m.folder";

fn summary(r: &Row) -> rusqlite::Result<Summary> {
    let att: Vec<Attachment> = serde_json::from_str(&r.get::<_, String>(11)?).unwrap_or_default();
    Ok(Summary {
        id: r.get(0)?, account: r.get(1)?, role: Role::from_db(&r.get::<_, String>(2)?), folder_path: r.get(3)?, date: r.get(4)?,
        from: Addr { name: r.get(5)?, addr: r.get(6)? }, subject: r.get(7)?, preview: r.get(8)?,
        seen: r.get(9)?, flagged: r.get(10)?, has_attachments: att.iter().any(|a| !a.inline), queued: false, failed: false, remote: None,
    })
}

impl Db {
    pub fn open(path: &Path) -> Result<Db, String> {
        use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
        let io = |x: std::io::Error| format!("{}: {x}", path.display());
        if let Some(dir) = path.parent() { std::fs::DirBuilder::new().recursive(true).mode(0o700).create(dir).map_err(io)?; }
        // Connections switching a fresh file to WAL at the same moment get "database is locked"
        // at once, busy_timeout or not. Only maild opens this file, so one opener at a time suffices.
        static OPEN: std::sync::Mutex<()> = std::sync::Mutex::new(());
        let _one_at_a_time = OPEN.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        // Private mail: a new file is created 0600 (SQLite gives its -wal and -shm the same mode).
        std::fs::OpenOptions::new().append(true).create(true).mode(0o600).open(path).map_err(io)?;
        Self::init(Connection::open(path).map_err(e)?)
    }

    #[cfg(test)]
    pub fn memory() -> Db { Self::init(Connection::open_in_memory().unwrap()).unwrap() }

    fn init(c: Connection) -> Result<Db, String> {
        c.busy_timeout(std::time::Duration::from_millis(5000)).map_err(e)?;
        c.pragma_update(None, "foreign_keys", "ON").map_err(e)?;
        c.pragma_update(None, "journal_mode", "WAL").map_err(e)?;
        c.create_scalar_function("mail_strip", 1, FunctionFlags::SQLITE_UTF8 | FunctionFlags::SQLITE_DETERMINISTIC, |ctx| {
            Ok(ctx.get::<Option<String>>(0)?.map(|h| mime::strip_tags(&h)).unwrap_or_default())
        }).map_err(e)?;
        // The FTS index holds names and addresses, not the JSON around them.
        c.create_scalar_function("mail_addrs", 1, FunctionFlags::SQLITE_UTF8 | FunctionFlags::SQLITE_DETERMINISTIC, |ctx| {
            let to: Vec<Addr> = ctx.get::<Option<String>>(0)?.and_then(|j| serde_json::from_str(&j).ok()).unwrap_or_default();
            Ok(to.iter().map(|a| format!("{} {}", a.name, a.addr)).collect::<Vec<_>>().join(" "))
        }).map_err(e)?;
        // A later schema change adds a step here (`1 => migrate to 2`), run once per database.
        match c.pragma_query_value(None, "user_version", |r| r.get::<_, i64>(0)).map_err(e)? {
            0 => c.execute_batch(&format!("BEGIN; {SCHEMA} PRAGMA user_version = 1; COMMIT;")).map_err(e)?,
            1 => {}
            v => return Err(format!("database schema version {v} is newer than this maild")),
        }
        Ok(Db { c })
    }

    pub fn upsert_folder(&self, account: &str, path: &str, role: Role) -> Result<i64, String> {
        self.c.query_row(
            "INSERT INTO folders(account, path, role) VALUES (?1, ?2, ?3)
             ON CONFLICT(account, path) DO UPDATE SET role = excluded.role RETURNING id",
            params![account, path, role.as_str()], |r| r.get(0)).map_err(e)
    }

    pub fn folders_of(&self, account: &str) -> Result<Vec<Folder>, String> {
        let mut st = self.c.prepare("SELECT id, account, path, role, COALESCE(uidvalidity,0), last_uid, notified_uid FROM folders WHERE account = ?1 ORDER BY id").map_err(e)?;
        st.query_map([account], |r| Ok(Folder {
            id: r.get(0)?, account: r.get(1)?, path: r.get(2)?, role: Role::from_db(&r.get::<_, String>(3)?),
            uidvalidity: r.get(4)?, last_uid: r.get(5)?, notified_uid: r.get(6)?,
        })).map_err(e)?.collect::<Result<_, _>>().map_err(e)
    }

    pub fn remove_folder(&self, id: i64) -> Result<(), String> {
        self.c.execute("DELETE FROM folders WHERE id = ?1", [id]).map(|_| ()).map_err(e)
    }

    /// UIDVALIDITY changed: drop the messages, restart UID tracking (`notified_uid` stays).
    /// One transaction: a crash in between must not keep the old `last_uid` with the messages gone.
    pub fn reset_folder(&self, id: i64, uidvalidity: u32) -> Result<(), String> {
        let tx = self.c.unchecked_transaction().map_err(e)?;
        tx.execute("DELETE FROM messages WHERE folder = ?1", [id]).map_err(e)?;
        tx.execute("UPDATE folders SET uidvalidity = ?2, last_uid = 0 WHERE id = ?1", params![id, uidvalidity]).map_err(e)?;
        tx.commit().map_err(e)
    }

    pub fn min_uid(&self, folder: i64) -> Result<Option<u32>, String> {
        self.c.query_row("SELECT MIN(uid) FROM messages WHERE folder = ?1", [folder], |r| r.get(0)).map_err(e)
    }

    pub fn set_folder_marks(&self, id: i64, last_uid: u32, notified_uid: u32) -> Result<(), String> {
        self.c.execute("UPDATE folders SET last_uid = ?2, notified_uid = ?3 WHERE id = ?1", params![id, last_uid, notified_uid]).map(|_| ()).map_err(e)
    }

    pub fn insert_message(&self, folder: i64, uid: u32, seen: bool, flagged: bool, size: u64, p: &Parsed, has_body: bool) -> Result<i64, String> {
        let to = serde_json::to_string(&p.to).map_err(|x| x.to_string())?;
        let att = serde_json::to_string(&p.attachments).map_err(|x| x.to_string())?;
        self.c.query_row(
            "INSERT INTO messages(folder, uid, message_id, in_reply_to, refs, date, from_name, from_addr, to_json, subject, preview, text, html, has_body, size, seen, flagged, attachments_json)
             VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18)
             ON CONFLICT(folder, uid) DO UPDATE SET message_id=excluded.message_id, in_reply_to=excluded.in_reply_to, refs=excluded.refs,
               date=excluded.date, from_name=excluded.from_name, from_addr=excluded.from_addr, to_json=excluded.to_json, subject=excluded.subject,
               preview=excluded.preview, text=excluded.text, html=excluded.html, has_body=excluded.has_body, size=excluded.size,
               seen=excluded.seen, flagged=excluded.flagged, attachments_json=excluded.attachments_json
             RETURNING id",
            params![folder, uid, p.message_id, p.in_reply_to, p.references.join(" "), p.date, p.from.name, p.from.addr, to, p.subject,
                    p.preview, p.text, p.html, has_body, size as i64, seen, flagged, att],
            |r| r.get(0)).map_err(e)
    }

    /// Unchanged rows are not rewritten: every sync refreshes the flags of the whole folder. Nor are
    /// rows with a pending action: the server does not have that change yet.
    pub fn set_flags(&self, folder: i64, uid: u32, seen: bool, flagged: bool) -> Result<(), String> {
        self.c.execute(
            "UPDATE messages SET seen = ?3, flagged = ?4 WHERE folder = ?1 AND uid = ?2 AND (seen <> ?3 OR flagged <> ?4)
             AND NOT EXISTS (SELECT 1 FROM actions WHERE message = messages.id AND state = 'pending')",
            params![folder, uid, seen, flagged]).map(|_| ()).map_err(e)
    }

    /// Delete the folder's messages whose UID is not in `present`.
    pub fn remove_missing(&self, folder: i64, present: &[u32]) -> Result<(), String> {
        let tx = self.c.unchecked_transaction().map_err(e)?;
        tx.execute_batch("CREATE TEMP TABLE IF NOT EXISTS present(uid INTEGER PRIMARY KEY); DELETE FROM present;").map_err(e)?;
        {
            let mut ins = tx.prepare("INSERT OR IGNORE INTO present VALUES (?1)").map_err(e)?;
            for u in present { ins.execute([u]).map_err(e)?; }
        }
        tx.execute("DELETE FROM messages WHERE folder = ?1 AND uid IS NOT NULL AND uid NOT IN (SELECT uid FROM present)", [folder]).map_err(e)?;
        tx.commit().map_err(e)
    }

    pub fn list(&self, q: &ListQuery) -> Result<Vec<Summary>, String> {
        let mut conds: Vec<String> = vec![];
        let mut args: Vec<Value> = vec![];
        if let Some(a) = &q.account { conds.push("f.account = ?".into()); args.push(a.clone().into()); }
        match (q.role, q.folder) {
            (Some(Role::Flagged), _) => conds.push("m.flagged = 1 AND f.role NOT IN ('trash','junk')".into()),
            (Some(r), _) => { conds.push("f.role = ?".into()); args.push(r.as_str().to_string().into()); }
            (None, Some(f)) => { conds.push("f.id = ?".into()); args.push(f.into()); }
            _ => conds.push("f.role <> 'remote'".into()),
        }
        if q.unread_only { conds.push("m.seen = 0".into()); }
        let wh = if conds.is_empty() { String::new() } else { format!("WHERE {}", conds.join(" AND ")) };
        // Queued outbox rows form a virtual prefix of the Sent list: rows [0, n) are outbox.
        let queued = if q.role == Some(Role::Sent) && !q.unread_only { self.queued(q.account.as_deref())? } else { vec![] };
        let n = queued.len() as i64;
        let (offset, limit) = (q.offset as i64, if q.limit == 0 { u32::MAX } else { q.limit } as i64);
        let mut out: Vec<Summary> = queued.into_iter().skip(offset.min(n) as usize).take(limit as usize).collect();
        args.push((limit - out.len() as i64).into());
        args.push((offset - n).max(0).into());
        let sql = format!("SELECT {SUMMARY_COLS} {SUMMARY_FROM} {wh} ORDER BY m.date DESC, m.id DESC LIMIT ? OFFSET ?");
        let mut st = self.c.prepare(&sql).map_err(e)?;
        let rows = st.query_map(params_from_iter(args), summary).map_err(e)?;
        for r in rows { out.push(r.map_err(e)?); }
        Ok(out)
    }

    /// Outbox rows as summaries: negative `id`, `queued = true`, newest first.
    fn queued(&self, account: Option<&str>) -> Result<Vec<Summary>, String> {
        let mut st = self.c.prepare("SELECT id, account, date, subject, COALESCE(state, '') = 'failed', COALESCE(state, '') <> 'sent' FROM outbox WHERE (?1 IS NULL OR account = ?1) ORDER BY date DESC, id DESC").map_err(e)?;
        st.query_map([account], |r| {
            let acc: String = r.get(1)?;
            Ok(Summary {
                id: -r.get::<_, i64>(0)?, account: acc.clone(), role: Role::Sent, folder_path: String::new(), date: r.get(2)?,
                from: Addr { name: String::new(), addr: acc }, subject: r.get(3)?, preview: String::new(),
                seen: true, flagged: false, has_attachments: false, queued: r.get(5)?, failed: r.get(4)?, remote: None,
            })
        }).map_err(e)?.collect::<Result<_, _>>().map_err(e)
    }

    pub fn folder_list(&self, account: Option<&str>) -> Result<Vec<FolderInfo>, String> {
        let unread = |cond: &str, acc: Option<&str>| -> Result<u32, String> {
            self.c.query_row(
                &format!("SELECT COUNT(*) FROM messages m JOIN folders f ON f.id = m.folder WHERE m.seen = 0 AND {cond} AND (?1 IS NULL OR f.account = ?1)"),
                [acc], |r| r.get(0)).map_err(e)
        };
        if let Some(a) = account {
            let mut st = self.c.prepare("SELECT id, path, role, (SELECT COUNT(*) FROM messages WHERE folder = folders.id AND seen = 0) FROM folders WHERE account = ?1 AND role <> 'remote' ORDER BY id").map_err(e)?;
            return st.query_map([a], |r| {
                let path: String = r.get(1)?;
                Ok(FolderInfo { account: Some(a.to_string()), id: Some(r.get(0)?), name: display_name(&path), path, role: Role::from_db(&r.get::<_, String>(2)?), unread: r.get(3)? })
            }).map_err(e)?.collect::<Result<_, _>>().map_err(e);
        }
        [Role::Inbox, Role::Flagged, Role::Sent, Role::Trash, Role::Junk].into_iter().map(|role| {
            let cond = match role {
                Role::Flagged => "m.flagged = 1 AND f.role NOT IN ('trash','junk')".to_string(),
                r => format!("f.role = '{}'", r.as_str()),
            };
            Ok(FolderInfo { account: None, id: None, path: role.as_str().into(), name: role.as_str().into(), role, unread: unread(&cond, None)? })
        }).collect()
    }

    pub fn message(&self, id: i64) -> Result<Option<Full>, String> {
        let sql = format!("SELECT {SUMMARY_COLS}, m.to_json, m.text, m.html, m.has_body, m.message_id, m.in_reply_to, m.refs, m.folder, m.uid {SUMMARY_FROM} WHERE m.id = ?1");
        match self.c.query_row(&sql, [id], |r| {
            Ok(Full {
                summary: summary(r)?,
                to: serde_json::from_str(&r.get::<_, String>(12)?).unwrap_or_default(),
                text: r.get(13)?, html: r.get(14)?,
                attachments: serde_json::from_str(&r.get::<_, String>(11)?).unwrap_or_default(),
                has_body: r.get(15)?, message_id: r.get(16)?, in_reply_to: r.get(17)?, refs: r.get(18)?, folder: r.get(19)?, uid: r.get(20)?,
            })
        }) {
            Ok(f) => Ok(Some(f)),
            Err(rusqlite::Error::QueryReturnedNoRows) => Ok(None),
            Err(x) => Err(e(x)),
        }
    }

    pub fn set_body(&self, id: i64, text: &str, html: Option<&str>, preview: &str) -> Result<(), String> {
        self.c.execute("UPDATE messages SET text = ?2, html = ?3, preview = ?4, has_body = 1 WHERE id = ?1", params![id, text, html, preview]).map(|_| ()).map_err(e)
    }

    pub fn set_attachments(&self, id: i64, atts: &[Attachment]) -> Result<(), String> {
        let json = serde_json::to_string(atts).map_err(|x| x.to_string())?;
        self.c.execute("UPDATE messages SET attachments_json = ?2 WHERE id = ?1", params![id, json]).map(|_| ()).map_err(e)
    }

    pub fn search(&self, q: &str, limit: u32) -> Result<Vec<Summary>, String> {
        let m = q.split_whitespace().map(|t| format!("\"{}\"*", t.replace('"', "\"\""))).collect::<Vec<_>>().join(" AND ");
        if m.is_empty() { return Ok(vec![]); }
        let sql = format!("SELECT {SUMMARY_COLS} {SUMMARY_FROM} WHERE m.id IN (SELECT rowid FROM messages_fts WHERE messages_fts MATCH ?1) ORDER BY m.date DESC LIMIT ?2");
        let mut st = self.c.prepare(&sql).map_err(e)?;
        st.query_map(params![m, limit], summary).map_err(e)?.collect::<Result<_, _>>().map_err(e)
    }

    /// Senders and recipients of mail in Sent folders, most frequent first.
    pub fn contacts(&self, prefix: &str, limit: u32) -> Result<Vec<Addr>, String> {
        let mut st = self.c.prepare("SELECT m.from_name, m.from_addr, m.to_json FROM messages m JOIN folders f ON f.id = m.folder WHERE f.role = 'sent'").map_err(e)?;
        let rows = st.query_map([], |r| Ok((Addr { name: r.get(0)?, addr: r.get(1)? }, r.get::<_, String>(2)?))).map_err(e)?;
        let mut seen: HashMap<String, (Addr, u32)> = HashMap::new();
        for r in rows {
            let (from, to) = r.map_err(e)?;
            let to: Vec<Addr> = serde_json::from_str(&to).unwrap_or_default();
            for a in std::iter::once(from).chain(to).filter(|a| !a.addr.is_empty()) {
                let slot = seen.entry(a.addr.to_lowercase()).or_insert((a.clone(), 0));
                slot.1 += 1;
                if slot.0.name.is_empty() { slot.0.name = a.name; }
            }
        }
        let p = prefix.to_lowercase();
        let mut v: Vec<_> = seen.into_values().filter(|(a, _)| a.addr.to_lowercase().contains(&p) || a.name.to_lowercase().contains(&p)).collect();
        v.sort_by(|x, y| y.1.cmp(&x.1).then_with(|| x.0.addr.cmp(&y.0.addr)));
        Ok(v.into_iter().take(limit as usize).map(|x| x.0).collect())
    }

    /// Queues a built message; the id (positive here, negative in `list`).
    pub fn outbox_add(&self, account: &str, raw: &[u8], to: &[Addr], subject: &str, date: i64) -> Result<i64, String> {
        let to_json = serde_json::to_string(to).map_err(|x| x.to_string())?;
        self.c.execute("INSERT INTO outbox(account, raw, to_json, subject, date, state) VALUES (?1, ?2, ?3, ?4, ?5, 'queued')",
            params![account, raw, to_json, subject, date]).map_err(e)?;
        Ok(self.c.last_insert_rowid())
    }

    /// `(id, account, recipients, raw)` of the rows still to send, in id order.
    pub fn outbox_queued(&self) -> Result<Vec<(i64, String, Vec<Addr>, Vec<u8>)>, String> {
        let mut st = self.c.prepare("SELECT id, account, to_json, raw FROM outbox WHERE state = 'queued' ORDER BY id").map_err(e)?;
        st.query_map([], |r| Ok((r.get(0)?, r.get(1)?, serde_json::from_str(&r.get::<_, String>(2)?).unwrap_or_default(), r.get(3)?)))
            .map_err(e)?.collect::<Result<_, _>>().map_err(e)
    }

    /// The server has the message: it must never go out again. Stays in Sent until its copy is filed.
    pub fn outbox_sent(&self, id: i64) -> Result<(), String> {
        self.c.execute("UPDATE outbox SET state = 'sent' WHERE id = ?1", [id]).map(drop).map_err(e)
    }

    /// `(id, raw)` of the account's sent rows whose copy is not in its Sent folder yet.
    pub fn outbox_unfiled(&self, account: &str) -> Result<Vec<(i64, Vec<u8>)>, String> {
        let mut st = self.c.prepare("SELECT id, raw FROM outbox WHERE account = ?1 AND state = 'sent' ORDER BY id").map_err(e)?;
        st.query_map([account], |r| Ok((r.get(0)?, r.get(1)?))).map_err(e)?.collect::<Result<_, _>>().map_err(e)
    }

    /// failed -> queued.
    pub fn outbox_retry(&self, id: i64) -> Result<(), String> {
        match self.c.execute("UPDATE outbox SET state = 'queued', error = NULL WHERE id = ?1 AND state = 'failed'", [id]).map_err(e)? {
            0 => Err("not a failed message".into()),
            _ => Ok(()),
        }
    }

    /// Drops a queued or failed row.
    pub fn outbox_discard(&self, id: i64) -> Result<(), String> {
        match self.c.execute("DELETE FROM outbox WHERE id = ?1 AND COALESCE(state, '') IN ('queued', 'failed')", [id]).map_err(e)? {
            0 => Err("message not found".into()),
            _ => Ok(()),
        }
    }

    /// An outbox row as a message (`id` = `-id`), parsed from its raw text; with the error of a failed one.
    pub fn outbox_full(&self, id: i64) -> Result<Option<(Full, Option<String>)>, String> {
        let row = self.c.query_row("SELECT account, raw, date, COALESCE(state, ''), error FROM outbox WHERE id = ?1", [id],
            |r| Ok((r.get::<_, String>(0)?, r.get::<_, Vec<u8>>(1)?, r.get::<_, i64>(2)?, r.get::<_, String>(3)?, r.get::<_, Option<String>>(4)?)));
        let (account, raw, date, state, error) = match row {
            Ok(x) => x,
            Err(rusqlite::Error::QueryReturnedNoRows) => return Ok(None),
            Err(x) => return Err(e(x)),
        };
        let p = mime::parse(&raw);
        let summary = Summary {
            id: -id, account, role: Role::Sent, folder_path: String::new(), date, from: p.from, subject: p.subject, preview: p.preview,
            seen: true, flagged: false, has_attachments: p.attachments.iter().any(|a| !a.inline),
            queued: state != "sent", failed: state == "failed", remote: None,
        };
        let full = Full {
            summary, to: p.to, text: p.text, html: p.html, attachments: p.attachments, has_body: true,
            message_id: p.message_id, in_reply_to: p.in_reply_to, refs: p.references.join(" "), folder: 0, uid: None,
        };
        Ok(Some((full, error)))
    }

    pub fn outbox_done(&self, id: i64) -> Result<(), String> {
        self.c.execute("DELETE FROM outbox WHERE id = ?1", [id]).map(drop).map_err(e)
    }

    pub fn outbox_fail(&self, id: i64, error: &str) -> Result<(), String> {
        self.c.execute("UPDATE outbox SET state = 'failed', error = ?2 WHERE id = ?1", params![id, error]).map(drop).map_err(e)
    }

    /// What still has to reach the server: pending actions + queued outbox rows.
    pub fn queued_count(&self, account: &str) -> Result<u32, String> {
        self.c.query_row(
            "SELECT (SELECT COUNT(*) FROM actions WHERE account = ?1 AND state = 'pending') + (SELECT COUNT(*) FROM outbox WHERE account = ?1 AND state = 'queued')",
            [account], |r| r.get(0)).map_err(e)
    }

    /// Everything of the account: folders (their messages cascade), actions, outbox.
    pub fn remove_account(&self, account: &str) -> Result<(), String> {
        let tx = self.c.unchecked_transaction().map_err(e)?;
        for t in ["folders", "actions", "outbox"] {
            tx.execute(&format!("DELETE FROM {t} WHERE account = ?1"), [account]).map_err(e)?;
        }
        tx.commit().map_err(e)
    }

    /// Unread Inbox mail with `uid > notified_uid`, for notifications. A message restored from
    /// Trash lands under a new uid but is not new mail.
    pub fn unread_inbox(&self, account: &str) -> Result<Vec<Summary>, String> {
        let sql = format!("SELECT {SUMMARY_COLS} {SUMMARY_FROM} WHERE f.account = ?1 AND f.role = 'inbox' AND m.seen = 0 AND m.uid > f.notified_uid
                           AND NOT EXISTS (SELECT 1 FROM actions a WHERE a.message = m.id AND a.op = 'restore') ORDER BY m.uid");
        let mut st = self.c.prepare(&sql).map_err(e)?;
        st.query_map([account], summary).map_err(e)?.collect::<Result<_, _>>().map_err(e)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(subject: &str, date: i64, text: &str) -> Parsed {
        Parsed { subject: subject.into(), date, text: text.into(), from: Addr { name: "A".into(), addr: "a@x.org".into() }, ..Default::default() }
    }
    fn q(role: Option<Role>) -> ListQuery { ListQuery { role, limit: 50, ..Default::default() } }

    #[test]
    fn insert_and_list() {
        let db = Db::memory();
        let f = db.upsert_folder("me@x.org", "INBOX", Role::Inbox).unwrap();
        db.insert_message(f, 1, false, false, 10, &mime::parse(include_bytes!("../tests/fixtures/plain.eml")), true).unwrap();
        db.insert_message(f, 2, true, false, 10, &p("newer", 2_000_000_000, "x"), true).unwrap();
        let l = db.list(&ListQuery { folder: Some(f), limit: 10, ..Default::default() }).unwrap();
        assert_eq!(l.len(), 2);
        assert_eq!(l[0].subject, "newer");
        assert_eq!(l[1].subject, "Hello plain");
        let again = db.insert_message(f, 2, true, false, 10, &p("newer2", 2_000_000_000, "x"), true).unwrap();
        assert_eq!(db.message(again).unwrap().unwrap().summary.subject, "newer2");
        assert_eq!(db.list(&q(Some(Role::Inbox))).unwrap().len(), 2);
    }

    #[test]
    fn unified_inbox() {
        let db = Db::memory();
        for (acc, uid) in [("a@x", 1), ("b@x", 1)] {
            let f = db.upsert_folder(acc, "INBOX", Role::Inbox).unwrap();
            db.insert_message(f, uid, false, false, 1, &p("s", 5, "t"), true).unwrap();
        }
        assert_eq!(db.list(&q(Some(Role::Inbox))).unwrap().len(), 2);
        let fl = db.folder_list(None).unwrap();
        assert_eq!(fl.iter().map(|f| f.role).collect::<Vec<_>>(), [Role::Inbox, Role::Flagged, Role::Sent, Role::Trash, Role::Junk]);
        assert_eq!(fl[0].unread, 2);
    }

    #[test]
    fn flagged_excludes_trash() {
        let db = Db::memory();
        let i = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let t = db.upsert_folder("a@x", "Trash", Role::Trash).unwrap();
        db.insert_message(i, 1, true, true, 1, &p("keep", 5, "t"), true).unwrap();
        db.insert_message(t, 1, true, true, 1, &p("gone", 5, "t"), true).unwrap();
        let l = db.list(&q(Some(Role::Flagged))).unwrap();
        assert_eq!(l.len(), 1);
        assert_eq!(l[0].subject, "keep");
    }

    #[test]
    fn search_prefix_cyrillic() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        db.insert_message(f, 1, true, false, 1, &p("s", 5, "Прошу подписать по договору поставки"), true).unwrap();
        assert_eq!(db.search("догов", 10).unwrap().len(), 1);
        assert_eq!(db.search("ДОГОВ", 10).unwrap().len(), 1);
        assert_eq!(db.search("договор постав", 10).unwrap().len(), 1);
        assert!(db.search("   ", 10).unwrap().is_empty());
    }

    #[test]
    fn search_html_body_without_tags() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let mut m = p("s", 5, "");
        m.html = Some("<div><b>счёт</b> оплачен</div>".into());
        db.insert_message(f, 1, true, false, 1, &m, true).unwrap();
        assert_eq!(db.search("оплач", 10).unwrap().len(), 1);
        assert!(db.search("div", 10).unwrap().is_empty());
    }

    #[test]
    fn search_quotes_safe() {
        let db = Db::memory();
        assert!(db.search("\"; DROP", 10).is_ok());
    }

    #[test]
    fn remove_folder_cascades() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let id = db.insert_message(f, 1, true, false, 1, &p("s", 5, "уникальное слово"), true).unwrap();
        db.remove_folder(f).unwrap();
        assert!(db.message(id).unwrap().is_none());
        assert!(db.search("уникальн", 10).unwrap().is_empty());
    }

    #[test]
    fn outbox_in_sent() {
        let db = Db::memory();
        db.c.execute("INSERT INTO outbox(account, raw, subject, date, state) VALUES ('a@x', x'00', 'queued one', 9, 'queued')", []).unwrap();
        let f = db.upsert_folder("a@x", "Sent", Role::Sent).unwrap();
        db.insert_message(f, 1, true, false, 1, &p("old", 5, "t"), true).unwrap();
        let l = db.list(&q(Some(Role::Sent))).unwrap();
        assert_eq!(l.len(), 2);
        assert!(l[0].queued && l[0].id < 0 && l[0].subject == "queued one");
        assert!(!l[1].queued);
    }

    #[test]
    fn outbox_pagination() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "Sent", Role::Sent).unwrap();
        for i in 1..=2 { db.c.execute("INSERT INTO outbox(account, raw, subject, date) VALUES ('a@x', x'00', ?1, 99)", [format!("q{i}")]).unwrap(); }
        for i in 1..=5 { db.insert_message(f, i, true, false, 1, &p(&format!("m{i}"), 100 - i as i64, "t"), true).unwrap(); }
        let page = |o| db.list(&ListQuery { role: Some(Role::Sent), offset: o, limit: 3, ..Default::default() }).unwrap()
            .into_iter().map(|s| s.subject).collect::<Vec<_>>();
        assert_eq!(page(0), ["q2", "q1", "m1"]);
        assert_eq!(page(3), ["m2", "m3", "m4"]);
        assert_eq!(page(6), ["m5"]);
    }

    #[test]
    fn fts_follows_updates() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let id = db.insert_message(f, 1, false, false, 1, &p("s", 5, "alpha"), true).unwrap();
        db.set_body(id, "beta", None, "beta").unwrap();
        assert!(db.search("alpha", 10).unwrap().is_empty());
        assert_eq!(db.search("beta", 10).unwrap().len(), 1);
        db.set_flags(f, 1, true, true).unwrap();
        assert_eq!(db.search("beta", 10).unwrap().len(), 1);
    }

    #[test]
    fn queued_count_and_remove_account() {
        let db = Db::memory();
        for (acc, state) in [("a@x", "pending"), ("a@x", "done"), ("a@x", "failed"), ("b@x", "pending")] {
            db.c.execute("INSERT INTO actions(account, state) VALUES (?1, ?2)", [acc, state]).unwrap();
        }
        for (acc, state) in [("a@x", "queued"), ("a@x", "failed")] {
            db.c.execute("INSERT INTO outbox(account, raw, state) VALUES (?1, x'00', ?2)", [acc, state]).unwrap();
        }
        assert_eq!(db.queued_count("a@x").unwrap(), 2);
        assert_eq!(db.queued_count("nobody").unwrap(), 0);
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let id = db.insert_message(f, 1, true, false, 1, &p("s", 5, "прощальное"), true).unwrap();
        let other = db.upsert_folder("b@x", "INBOX", Role::Inbox).unwrap();
        db.remove_account("a@x").unwrap();
        assert!(db.folders_of("a@x").unwrap().is_empty() && db.message(id).unwrap().is_none());
        assert!(db.search("прощальн", 10).unwrap().is_empty());
        assert_eq!(db.queued_count("a@x").unwrap(), 0);
        let left: i64 = db.c.query_row("SELECT (SELECT COUNT(*) FROM actions) + (SELECT COUNT(*) FROM outbox)", [], |r| r.get(0)).unwrap();
        assert_eq!(left, 1);
        assert_eq!(db.folders_of("b@x").unwrap()[0].id, other);
    }

    #[test]
    fn contacts_ranked() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "Sent", Role::Sent).unwrap();
        for uid in 1..=4 {
            let who = if uid <= 3 { "often@y.org" } else { "once@y.org" };
            let mut m = p("s", uid as i64, "t");
            m.to = vec![Addr { name: String::new(), addr: who.into() }];
            db.insert_message(f, uid, true, false, 1, &m, true).unwrap();
        }
        let c = db.contacts("y.org", 10).unwrap();
        assert_eq!(c.iter().map(|a| a.addr.as_str()).collect::<Vec<_>>(), ["often@y.org", "once@y.org"]);
    }

    #[test]
    fn marks_flags_and_missing() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        for uid in 1..=3 { db.insert_message(f, uid, false, false, 1, &p("s", uid as i64, "t"), true).unwrap(); }
        db.set_flags(f, 1, true, true).unwrap();
        db.remove_missing(f, &[1, 3]).unwrap();
        assert_eq!(db.list(&q(Some(Role::Inbox))).unwrap().len(), 2);
        db.set_folder_marks(f, 3, 1).unwrap();
        assert_eq!(db.unread_inbox("a@x").unwrap().len(), 1);
        db.reset_folder(f, 7).unwrap();
        let fo = &db.folders_of("a@x").unwrap()[0];
        assert_eq!((fo.uidvalidity, fo.last_uid, fo.notified_uid), (7, 0, 1));
    }

    #[test]
    fn folder_names_decoded() {
        let db = Db::memory();
        db.upsert_folder("a@x", "[Gmail]/&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-", Role::Sent).unwrap();
        let f = &db.folder_list(Some("a@x")).unwrap()[0];
        assert_eq!((f.name.as_str(), f.path.as_str()), ("Отправленные", "[Gmail]/&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-"));
        assert_eq!(db.folder_list(None).unwrap()[0].name, "inbox");
    }

    #[test]
    fn ids_are_never_reused() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let old = db.insert_message(f, 1, false, false, 1, &p("s", 5, "t"), true).unwrap();
        db.remove_missing(f, &[]).unwrap(); // the newest row goes
        assert_ne!(db.insert_message(f, 2, false, false, 1, &p("s", 6, "t"), true).unwrap(), old);
        let o = db.outbox_add("a@x", b"x", &[], "s", 1).unwrap();
        db.outbox_done(o).unwrap();
        assert_ne!(db.outbox_add("a@x", b"x", &[], "s", 1).unwrap(), o);
    }

    #[test]
    fn schema_version_and_private_files() {
        use std::os::unix::fs::PermissionsExt;
        let mode = |p: &Path| std::fs::metadata(p).unwrap().permissions().mode() & 0o777;
        let v: i64 = Db::memory().c.pragma_query_value(None, "user_version", |r| r.get(0)).unwrap();
        assert_eq!(v, 1);
        let dir = std::env::temp_dir().join(format!("maild-test-{}-version", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let path = dir.join("data").join("mail.db");
        drop(Db::open(&path).unwrap());
        assert_eq!((mode(path.parent().unwrap()), mode(&path)), (0o700, 0o600));
        drop(Db::open(&path).unwrap()); // version 1 opens as it is
        Connection::open(&path).unwrap().pragma_update(None, "user_version", 99).unwrap();
        let e = Db::open(&path).err().unwrap();
        assert!(e.contains("99"), "{e}");
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn fts_indexes_names_and_addresses_not_json() {
        let db = Db::memory();
        let f = db.upsert_folder("a@x", "INBOX", Role::Inbox).unwrap();
        let mut m = p("s", 5, "t");
        m.to = vec![Addr { name: "Иван Петров".into(), addr: "ivan@y.org".into() }];
        db.insert_message(f, 1, true, false, 1, &m, true).unwrap();
        assert_eq!(db.search("петров", 10).unwrap().len(), 1);
        assert_eq!(db.search("ivan", 10).unwrap().len(), 1);
        assert!(db.search("name", 10).unwrap().is_empty() && db.search("addr", 10).unwrap().is_empty());
        m.to = vec![Addr { name: String::new(), addr: "olga@y.org".into() }];
        db.insert_message(f, 1, true, false, 1, &m, true).unwrap(); // the update path
        assert!(db.search("петров", 10).unwrap().is_empty());
        assert_eq!(db.search("olga", 10).unwrap().len(), 1);
    }

    #[test]
    fn concurrent_open_of_fresh_file() {
        for round in 0..10 {
            let dir = std::env::temp_dir().join(format!("maild-test-{}-open{round}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            let p = dir.join("mail.db");
            let opens: Vec<_> = (0..4).map(|_| { let p = p.clone(); std::thread::spawn(move || Db::open(&p).err()) }).collect();
            for t in opens { assert_eq!(t.join().unwrap(), None); }
            std::fs::remove_dir_all(&dir).unwrap();
        }
    }
}
