//! What the user does to mail (delete, read, flag, …). The database changes at once; the server
//! follows from a queue that survives restarts: `actions` rows, `pending` → `done` | `failed`.
//! Each row records where its message was when it was enqueued, so the worker replays the local
//! history on the server in `id` order, also after a time offline.

use crate::account::Job;
use crate::db::{Db, Role};
use crate::hub::{Event, Hub};
use crate::i18n::tr;
use crate::imap::{self, Conn};
use crate::sync;
use rusqlite::{Connection, OptionalExtension, Transaction, TransactionBehavior, params};
use serde::{Deserialize, Serialize};
use serde_json::Value;

/// `Restore` (back from Trash) comes from `undo` only.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Op { Delete, DeleteForever, Read, Unread, Flag, Unflag, Restore }

impl Op {
    /// As stored in `actions.op`: the protocol name.
    fn name(self) -> String {
        serde_json::to_value(self).ok().and_then(|v| v.as_str().map(str::to_string)).unwrap_or_default()
    }

    fn parse(name: String) -> Option<Op> {
        serde_json::from_value(Value::String(name)).ok()
    }

    fn inverse(self) -> Option<Op> {
        Some(match self {
            Op::Delete => Op::Restore,
            Op::Restore => Op::Delete,
            Op::Read => Op::Unread,
            Op::Unread => Op::Read,
            Op::Flag => Op::Unflag,
            Op::Unflag => Op::Flag,
            Op::DeleteForever => return None,
        })
    }
}

/// What a failed server command means for its action.
#[derive(Debug, PartialEq)]
pub enum Outcome {
    /// The message or its folder is not on the server any more: nothing left to do.
    Missing,
    /// The connection: try again later.
    Retry,
    /// The server will not do it.
    Reject,
}

pub fn classify(e: &imap::Error) -> Outcome {
    match e {
        imap::Error::Broken(_) => Outcome::Retry,
        // RFC 5530 codes; the parser leaves them in the text.
        imap::Error::Refused(text) => match text.strip_prefix('[').and_then(|t| t.split(']').next()).unwrap_or_default().to_ascii_uppercase().as_str() {
            "NONEXISTENT" | "EXPUNGEISSUED" => Outcome::Missing,
            "UNAVAILABLE" | "INUSE" => Outcome::Retry,
            _ => Outcome::Reject,
        },
    }
}

fn sql(e: rusqlite::Error) -> String { e.to_string() }

/// IMMEDIATE takes the write lock first: a concurrent writer (a sync) makes it wait instead of failing.
fn begin(db: &Db) -> Result<Transaction<'_>, String> {
    Transaction::new_unchecked(&db.c, TransactionBehavior::Immediate).map_err(sql)
}

fn rows<T>(db: &Db, query: &str, p: impl rusqlite::Params, f: impl FnMut(&rusqlite::Row) -> rusqlite::Result<T>) -> Result<Vec<T>, String> {
    let mut st = db.c.prepare(query).map_err(sql)?;
    st.query_map(p, f).map_err(sql)?.collect::<Result<_, _>>().map_err(sql)
}

/// Puts message `m` into `folder` as `uid`. A row already there under that uid is the same server
/// message: it goes.
fn place(c: &Connection, m: i64, folder: i64, uid: Option<u32>) -> Result<(), String> {
    if let Some(u) = uid {
        c.execute("DELETE FROM messages WHERE folder = ?1 AND uid = ?2 AND id <> ?3", params![folder, u, m]).map_err(sql)?;
    }
    c.execute("UPDATE messages SET folder = ?2, uid = ?3 WHERE id = ?1", params![m, folder, uid]).map(drop).map_err(sql)
}

/// Message `m` still waits in `folder` for its server uid, as a move left it.
fn awaiting(c: &Connection, m: i64, folder: i64) -> Result<bool, String> {
    c.query_row("SELECT EXISTS(SELECT 1 FROM messages WHERE id = ?1 AND folder = ?2 AND uid IS NULL)", params![m, folder], |r| r.get(0)).map_err(sql)
}

/// Applies `op` to the messages and queues it for their servers, in one transaction: an error
/// changes nothing. Messages already in the wanted state are left out, so that `undo` reverts only
/// what changed. Returns the batch for `undo`, 0 if nothing changed.
pub fn enqueue(db: &Db, ids: &[i64], op: Op) -> Result<i64, String> {
    // Outbox rows are negative, server search hits 0.
    if ids.iter().any(|&id| id <= 0) {
        return Err(tr("this message is not in a mailbox yet", "этого письма ещё нет в ящике"));
    }
    let tx = begin(db)?;
    let batch: i64 = tx.query_row("SELECT COALESCE(MAX(batch), 0) + 1 FROM actions", [], |r| r.get(0)).map_err(sql)?;
    let mut changed = false;
    for &id in ids {
        let msg = tx.query_row(
            "SELECT m.folder, m.uid, m.message_id, m.seen, m.flagged, f.account, f.role FROM messages m JOIN folders f ON f.id = m.folder WHERE m.id = ?1",
            [id],
            |r| Ok((r.get::<_, i64>(0)?, r.get::<_, Option<u32>>(1)?, r.get::<_, String>(2)?, r.get::<_, bool>(3)?, r.get::<_, bool>(4)?, r.get::<_, String>(5)?, r.get::<_, String>(6)?)),
        ).optional().map_err(sql)?;
        // Gone meanwhile: a sync removed it.
        let Some((folder, uid, message_id, seen, flagged, account, role)) = msg else { continue };
        let in_trash = role == Role::Trash.as_str();
        let set = |col: &str, v: bool| tx.execute(&format!("UPDATE messages SET {col} = ?2 WHERE id = ?1"), params![id, v]).map(drop).map_err(sql);
        let to: Option<i64> = match op {
            Op::Read | Op::Unread if seen == (op == Op::Read) => continue,
            Op::Flag | Op::Unflag if flagged == (op == Op::Flag) => continue,
            Op::Delete | Op::Restore if in_trash == (op == Op::Delete) => continue,
            Op::Read | Op::Unread => { set("seen", op == Op::Read)?; None }
            Op::Flag | Op::Unflag => { set("flagged", op == Op::Flag)?; None }
            Op::DeleteForever if !in_trash && role != Role::Junk.as_str() => {
                return Err(tr("only mail in Trash or Spam can be deleted forever", "навсегда удаляются только письма из Корзины и Спама"));
            }
            Op::DeleteForever => {
                tx.execute("DELETE FROM messages WHERE id = ?1", [id]).map_err(sql)?;
                None
            }
            Op::Delete => {
                let trash = tx.query_row("SELECT id FROM folders WHERE account = ?1 AND role = 'trash' ORDER BY id", [&account], |r| r.get(0)).optional().map_err(sql)?;
                let trash = trash.ok_or_else(|| tr("this mailbox has no Trash folder", "в этом ящике нет Корзины"))?;
                place(&tx, id, trash, None)?;
                Some(trash)
            }
            // Back where its last delete took it from, if that folder is still there.
            Op::Restore => {
                let home = tx.query_row(
                    "SELECT a.from_folder FROM actions a JOIN folders f ON f.id = a.from_folder
                     WHERE a.message = ?1 AND a.op = 'delete' AND a.state <> 'failed' ORDER BY a.id DESC LIMIT 1",
                    [id], |r| r.get(0)).optional().map_err(sql)?;
                let Some(home) = home else { continue };
                place(&tx, id, home, None)?;
                Some(home)
            }
        };
        tx.execute(
            "INSERT INTO actions(batch, account, op, message, from_folder, to_folder, uid, message_id, state) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'pending')",
            params![batch, account, op.name(), id, folder, to, uid, message_id],
        ).map_err(sql)?;
        changed = true;
    }
    if !changed {
        return Ok(0);
    }
    tx.commit().map_err(sql)?;
    Ok(batch)
}

/// Reverts a batch with a new one (`Op::inverse`), on what is left of it. Deleting forever cannot
/// be undone.
pub fn undo(db: &Db, batch: i64) -> Result<i64, String> {
    let done = rows(db, "SELECT op, message FROM actions WHERE batch = ?1 AND state <> 'failed' ORDER BY id", [batch], |r| Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?)))?;
    // A batch is one op.
    let op = done.first().and_then(|(op, _)| Op::parse(op.clone())).ok_or_else(|| tr("nothing to undo", "нечего отменять"))?;
    let inverse = op.inverse().ok_or_else(|| tr("deleting forever cannot be undone", "удаление навсегда не отменить"))?;
    enqueue(db, &done.iter().map(|d| d.1).collect::<Vec<_>>(), inverse)
}

/// After `enqueue` and `undo`: the window reloads, the workers of the batch's mailboxes start on it.
pub fn kick(db: &Db, hub: &Hub, batch: i64) {
    let got = match rows(db, "SELECT account, message FROM actions WHERE batch = ?1 ORDER BY id", [batch], |r| Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?))) {
        Ok(v) if !v.is_empty() => v,
        Ok(_) => return,
        Err(e) => return eprintln!("actions: {e}"),
    };
    for h in hub.handles().iter().filter(|h| got.iter().any(|(a, _)| *a == h.email)) {
        let _ = h.jobs.send(Job::Actions);
    }
    hub.emit(Event::Changed { folders: true, ids: got.into_iter().map(|(_, m)| m).collect() });
}

pub fn has_pending(db: &Db, account: &str) -> Result<bool, String> {
    db.c.query_row("SELECT EXISTS(SELECT 1 FROM actions WHERE account = ?1 AND state = 'pending')", [account], |r| r.get(0)).map_err(sql)
}

/// A pending row with the paths of its folders (`None`: the folder is gone, its messages with it).
struct Pending {
    id: i64, batch: i64, op: Op, message: i64, from: i64, to: Option<i64>,
    from_path: Option<String>, to_path: Option<String>, uid: Option<u32>, message_id: String,
}

/// The worker: replays the account's pending actions on the server, in order. A connection failure
/// ends it with `Err`; that action and the ones after it stay pending for the next `Actions`.
pub fn run(conn: &mut Conn, db: &Db, account: &str, hub: &Hub) -> Result<(), String> {
    let pending = rows(db,
        "SELECT a.id, a.batch, a.op, a.message, a.from_folder, a.to_folder, ff.path, tf.path, a.uid, a.message_id FROM actions a
         LEFT JOIN folders ff ON ff.id = a.from_folder LEFT JOIN folders tf ON tf.id = a.to_folder
         WHERE a.account = ?1 AND a.state = 'pending' ORDER BY a.id",
        [account],
        |r| {
            let Some(op) = Op::parse(r.get(2)?) else { return Ok(None) };
            Ok(Some(Pending {
                id: r.get(0)?, batch: r.get(1)?, op, message: r.get(3)?, from: r.get(4)?, to: r.get(5)?, from_path: r.get(6)?,
                to_path: r.get(7)?, uid: r.get(8)?, message_id: r.get::<_, Option<String>>(9)?.unwrap_or_default(),
            }))
        })?;
    let mut s = Server { conn, selected: None, caps: None };
    let mut told = None;
    for mut a in pending.into_iter().flatten() {
        let found = match execute(&mut s, &mut a) {
            Ok(found) => found,
            Err(e) => match classify(&e) {
                Outcome::Missing => None,
                Outcome::Retry => return Err(format!("{}: {e}", a.op.name())),
                Outcome::Reject => {
                    reject(&mut s, db, account, &a, &e.to_string())?;
                    // One event per batch: its rows come one after another.
                    if told != Some(a.batch) {
                        hub.emit(Event::ActionFailed { batch: a.batch, error: e.to_string() });
                        told = Some(a.batch);
                    }
                    continue;
                }
            },
        };
        settle(db, &a, found)?;
    }
    Ok(())
}

/// The server side of one action; fills in `a.uid` when it was unknown. `Ok(uid)`: where a move's
/// message is now in its target folder (`None` if not found there); `None` for the other actions.
fn execute(s: &mut Server, a: &mut Pending) -> Result<Option<u32>, imap::Error> {
    let Some(from) = &a.from_path else { return Ok(None) };
    s.select(from)?;
    // No uid: enqueued while an earlier move of the message was still on its way.
    if a.uid.is_none() {
        a.uid = s.find(&a.message_id)?;
    }
    let store = match a.op {
        Op::Delete | Op::Restore => {
            let Some(to) = &a.to_path else { return Ok(None) };
            // A gone uid: moved by an earlier try whose answer was lost, or by a phone.
            match a.uid.map(|u| s.mv(u, to)) {
                Some(Err(e)) if classify(&e) != Outcome::Missing => return Err(e),
                _ => {}
            }
            // Moved, or already there: a refusal from here on is not the move's.
            return match s.select(to).and_then(|()| s.find(&a.message_id)) {
                Err(e) if classify(&e) != Outcome::Retry => Ok(None),
                r => r,
            };
        }
        Op::Read => "+FLAGS.SILENT (\\Seen)",
        Op::Unread => "-FLAGS.SILENT (\\Seen)",
        Op::Flag => "+FLAGS.SILENT (\\Flagged)",
        Op::Unflag => "-FLAGS.SILENT (\\Flagged)",
        Op::DeleteForever => "+FLAGS.SILENT (\\Deleted)",
    };
    if let Some(u) = a.uid {
        s.conn.command(&format!("UID STORE {u} {store}"))?;
        if a.op == Op::DeleteForever {
            s.expunge(u)?;
        }
    }
    Ok(None)
}

/// Done on the server (or nothing to do there). A moved message still waiting in its target gets
/// its uid there, or goes when the target has no such message (the target's sync brings it).
fn settle(db: &Db, a: &Pending, found: Option<u32>) -> Result<(), String> {
    let tx = begin(db)?;
    if let Some(to) = a.to && awaiting(&tx, a.message, to)? {
        match found {
            Some(uid) => {
                place(&tx, a.message, to, Some(uid))?;
                // A restored message is not new mail.
                tx.execute("UPDATE folders SET notified_uid = MAX(notified_uid, ?2) WHERE id = ?1", params![to, uid]).map_err(sql)?;
            }
            None => {
                tx.execute("DELETE FROM messages WHERE id = ?1", [a.message]).map_err(sql)?;
            }
        }
    }
    tx.execute("UPDATE actions SET state = 'done' WHERE id = ?1", [a.id]).map_err(sql)?;
    tx.commit().map_err(sql)
}

/// The server refused: the local change is undone; a message deleted forever is fetched again.
fn reject(s: &mut Server, db: &Db, account: &str, a: &Pending, error: &str) -> Result<(), String> {
    let tx = begin(db)?;
    let set = |col: &str, v: bool| tx.execute(&format!("UPDATE messages SET {col} = ?2 WHERE id = ?1"), params![a.message, v]).map(drop).map_err(sql);
    match a.op {
        Op::Read | Op::Unread => set("seen", a.op == Op::Unread)?,
        Op::Flag | Op::Unflag => set("flagged", a.op == Op::Unflag)?,
        // Unless the message has moved on locally since.
        // ponytail: a refusal before the source uid was known puts it back without one, where no sync
        // touches it; needs a refused SELECT/SEARCH within a chain of offline moves.
        Op::Delete | Op::Restore => {
            if let Some(to) = a.to && awaiting(&tx, a.message, to)? {
                place(&tx, a.message, a.from, a.uid)?;
            }
        }
        // The folder's next sync starts just below its uid (from the start if it is unknown).
        Op::DeleteForever => {
            tx.execute("UPDATE folders SET last_uid = MIN(last_uid, ?2) WHERE id = ?1", params![a.from, a.uid.map_or(0, |u| u.saturating_sub(1))]).map_err(sql)?;
        }
    }
    tx.execute("UPDATE actions SET state = 'failed', error = ?2 WHERE id = ?1", params![a.id, error]).map_err(sql)?;
    tx.commit().map_err(sql)?;
    if a.op == Op::DeleteForever && let Some(f) = db.folders_of(account).unwrap_or_default().into_iter().find(|f| f.id == a.from) {
        // Before `action_failed`: the window then finds the message back.
        s.selected = None;
        if let Err(e) = sync::sync_folder(s.conn, db, &f) {
            eprintln!("{account}: {}: {e}", f.path);
        }
    }
    Ok(())
}

/// The worker's connection during one `run`: what is selected, what the server can do.
struct Server<'a> { conn: &'a mut Conn, selected: Option<String>, caps: Option<(bool, bool)> }

impl Server<'_> {
    /// SELECT, unless already selected.
    fn select(&mut self, path: &str) -> Result<(), imap::Error> {
        if self.selected.as_deref() != Some(path) {
            self.selected = None;
            self.conn.select(path)?;
            self.selected = Some(path.into());
        }
        Ok(())
    }

    /// (MOVE, UIDPLUS)
    fn caps(&mut self) -> Result<(bool, bool), imap::Error> {
        if let Some(c) = self.caps {
            return Ok(c);
        }
        let v = self.conn.capabilities()?;
        let c = (v.iter().any(|x| x == "MOVE"), v.iter().any(|x| x == "UIDPLUS"));
        self.caps = Some(c);
        Ok(c)
    }

    /// The newest message with this Message-ID in the selected folder.
    fn find(&mut self, message_id: &str) -> Result<Option<u32>, imap::Error> {
        match imap::quote(&format!("<{message_id}>")) {
            Some(q) if !message_id.is_empty() => Ok(self.conn.search(&format!("HEADER Message-ID {q}"))?.last().copied()),
            _ => Ok(None),
        }
    }

    /// `UID MOVE`; without MOVE: copy, mark \Deleted, expunge.
    fn mv(&mut self, uid: u32, to: &str) -> Result<(), imap::Error> {
        let q = imap::quote(to).ok_or_else(|| imap::Error::Refused(format!("{to}: bad folder name")))?;
        if self.caps()?.0 {
            return self.conn.command(&format!("UID MOVE {uid} {q}"));
        }
        // ponytail: a connection lost between COPY and EXPUNGE copies it again on the retry (a duplicate, nothing lost).
        self.conn.command(&format!("UID COPY {uid} {q}"))?;
        self.conn.command(&format!("UID STORE {uid} +FLAGS.SILENT (\\Deleted)"))?;
        self.expunge(uid)
    }

    /// Expunges `uid`, marked \Deleted. Without UIDPLUS a plain EXPUNGE takes every \Deleted message
    /// of the folder, so it runs only when `uid` is the only one.
    fn expunge(&mut self, uid: u32) -> Result<(), imap::Error> {
        if self.caps()?.1 {
            return self.conn.command(&format!("UID EXPUNGE {uid}"));
        }
        if self.conn.search("DELETED")? == [uid] {
            return self.conn.command("EXPUNGE");
        }
        eprintln!("{}: uid {uid} stays marked \\Deleted: no UIDPLUS, and other messages are marked too", self.selected.as_deref().unwrap_or_default());
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{ListQuery, Role};
    use crate::imap::{Error::{Broken, Refused}, fake_server};
    use crate::mime::Parsed;
    use std::net::TcpStream;
    use std::sync::{Arc, Mutex};

    const ME: &str = "me@x";
    const CAPS: &str = "* CAPABILITY IMAP4rev1 MOVE UIDPLUS\r\n";
    const FIND: &str = "UID SEARCH HEADER Message-ID \"<m1@x>\"";

    /// INBOX holding uid 5 (Message-ID m1@x, unread) and an empty Trash: (db, inbox, trash, message).
    fn setup() -> (Db, i64, i64, i64) {
        let db = Db::memory();
        let inbox = db.upsert_folder(ME, "INBOX", Role::Inbox).unwrap();
        let trash = db.upsert_folder(ME, "Trash", Role::Trash).unwrap();
        let p = Parsed { subject: "s".into(), message_id: "m1@x".into(), ..Default::default() };
        let id = db.insert_message(inbox, 5, false, false, 1, &p, true).unwrap();
        (db, inbox, trash, id)
    }

    fn ids_in(db: &Db, role: Role) -> Vec<i64> {
        db.list(&ListQuery { role: Some(role), ..Default::default() }).unwrap().into_iter().map(|s| s.id).collect()
    }

    fn place_of(db: &Db, id: i64) -> (i64, Option<u32>) {
        let m = db.message(id).unwrap().unwrap();
        (m.folder, m.uid)
    }

    fn states(db: &Db, batch: i64) -> Vec<String> {
        rows(db, "SELECT state FROM actions WHERE batch = ?1 ORDER BY id", [batch], |r| r.get(0)).unwrap()
    }

    type Log = Arc<Mutex<Vec<String>>>;

    /// A fake server answering `script` (LOGIN is always fine) and the commands it got.
    fn server(script: impl Fn(&str) -> Option<String> + Send + Sync + 'static) -> (Conn, Log) {
        let log = Log::default();
        let l = log.clone();
        let addr = fake_server(move |cmd| {
            if cmd.starts_with("LOGIN") {
                return Some(String::new());
            }
            l.lock().unwrap().push(cmd.to_string());
            script(cmd)
        });
        (Conn::plain(TcpStream::connect(addr).unwrap(), "me", "pw").unwrap(), log)
    }

    fn take(log: &Log) -> Vec<String> {
        std::mem::take(&mut *log.lock().unwrap())
    }

    #[test]
    fn delete_moves_locally() {
        let (db, _, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        assert!(b > 0);
        assert!(ids_in(&db, Role::Inbox).is_empty());
        assert_eq!(ids_in(&db, Role::Trash), [id]);
        assert_eq!(place_of(&db, id), (trash, None));
        assert_eq!(states(&db, b), ["pending"]);
        assert_eq!(db.queued_count(ME).unwrap(), 1);
        assert_eq!(enqueue(&db, &[id], Op::Delete).unwrap(), 0, "already in Trash: nothing to do");
    }

    #[test]
    fn delete_needs_trash() {
        let db = Db::memory();
        let inbox = db.upsert_folder(ME, "INBOX", Role::Inbox).unwrap();
        let id = db.insert_message(inbox, 5, false, false, 1, &Parsed::default(), true).unwrap();
        assert!(enqueue(&db, &[id], Op::Delete).is_err());
        assert_eq!(place_of(&db, id), (inbox, Some(5)));
        assert_eq!(db.queued_count(ME).unwrap(), 0);
    }

    #[test]
    fn undo_delete_restores_locally() {
        let (db, inbox, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let u = undo(&db, b).unwrap();
        assert!(u > b);
        assert_eq!(ids_in(&db, Role::Inbox), [id]);
        assert_eq!(place_of(&db, id), (inbox, None));
        let row = rows(&db, "SELECT op, from_folder, to_folder FROM actions WHERE batch = ?1", [u], |r| Ok((r.get::<_, String>(0)?, r.get(1)?, r.get(2)?))).unwrap();
        assert_eq!(row, [("restore".to_string(), trash, inbox)]);
        undo(&db, u).unwrap();
        assert_eq!(ids_in(&db, Role::Trash), [id], "undoing the undo deletes again");
        assert!(undo(&db, 999).is_err());
    }

    #[test]
    fn delete_forever_only_in_trash() {
        let (db, _, _, id) = setup();
        assert!(enqueue(&db, &[id], Op::DeleteForever).is_err());
        assert_eq!(ids_in(&db, Role::Inbox), [id]);
        assert_eq!(db.queued_count(ME).unwrap(), 0);
        enqueue(&db, &[id], Op::Delete).unwrap();
        let f = enqueue(&db, &[id], Op::DeleteForever).unwrap();
        assert!(db.message(id).unwrap().is_none());
        assert!(undo(&db, f).is_err());
    }

    #[test]
    fn read_flag_immediate() {
        let (db, inbox, _, id) = setup();
        let r = enqueue(&db, &[id], Op::Read).unwrap();
        enqueue(&db, &[id], Op::Flag).unwrap();
        let seen_flagged = || db.message(id).unwrap().map(|m| (m.summary.seen, m.summary.flagged)).unwrap();
        assert_eq!(seen_flagged(), (true, true));
        // a sync before the server has it must not undo the change
        db.set_flags(inbox, 5, false, false).unwrap();
        assert_eq!(seen_flagged(), (true, true));
        assert_eq!(enqueue(&db, &[id], Op::Read).unwrap(), 0, "already read");
        assert!(enqueue(&db, &[-1], Op::Read).is_err(), "outbox rows");

        let (mut c, log) = server(|cmd| matches!(cmd, "SELECT \"INBOX\"" | "UID STORE 5 +FLAGS.SILENT (\\Seen)" | "UID STORE 5 +FLAGS.SILENT (\\Flagged)").then(String::new));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(take(&log), ["SELECT \"INBOX\"", "UID STORE 5 +FLAGS.SILENT (\\Seen)", "UID STORE 5 +FLAGS.SILENT (\\Flagged)"]);
        assert_eq!(states(&db, r), ["done"]);
        assert_eq!(db.queued_count(ME).unwrap(), 0);
        // undo: only what the batch changed
        undo(&db, r).unwrap();
        assert_eq!(seen_flagged(), (false, true));
    }

    #[test]
    fn move_and_restore_on_server() {
        let (db, inbox, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let selected = Mutex::new(String::new());
        let (mut c, log) = server(move |cmd| {
            if let Some(path) = cmd.strip_prefix("SELECT ") {
                *selected.lock().unwrap() = path.into();
                return Some(String::new());
            }
            Some(match cmd {
                "CAPABILITY" => CAPS.into(),
                "UID MOVE 5 \"Trash\"" | "UID MOVE 9 \"INBOX\"" => String::new(),
                // in Trash it is uid 9; back in INBOX, 11
                FIND if *selected.lock().unwrap() == "\"Trash\"" => "* SEARCH 9\r\n".into(),
                FIND => "* SEARCH 11\r\n".into(),
                _ => return None,
            })
        });
        let hub = Hub::default();
        run(&mut c, &db, ME, &hub).unwrap();
        assert_eq!(take(&log), ["SELECT \"INBOX\"", "CAPABILITY", "UID MOVE 5 \"Trash\"", "SELECT \"Trash\"", FIND]);
        assert_eq!(place_of(&db, id), (trash, Some(9)));
        assert_eq!(states(&db, b), ["done"]);

        let u = undo(&db, b).unwrap();
        run(&mut c, &db, ME, &hub).unwrap();
        assert_eq!(take(&log), ["SELECT \"Trash\"", "CAPABILITY", "UID MOVE 9 \"INBOX\"", "SELECT \"INBOX\"", FIND]);
        assert_eq!(place_of(&db, id), (inbox, Some(11)));
        assert_eq!(states(&db, u), ["done"]);
        assert_eq!(db.folders_of(ME).unwrap()[0].notified_uid, 11, "a restored message is not new mail");
    }

    #[test]
    fn move_fallback_without_move() {
        let copy = ["SELECT \"INBOX\"", "CAPABILITY", "UID COPY 5 \"Trash\"", "UID STORE 5 +FLAGS.SILENT (\\Deleted)"];
        for (caps, deleted, expunge) in [
            (" UIDPLUS", "", &["UID EXPUNGE 5"][..]),
            ("", " 5", &["UID SEARCH DELETED", "EXPUNGE"][..]),
            // another message is marked \Deleted too: a plain EXPUNGE would take it
            ("", " 5 7", &["UID SEARCH DELETED"][..]),
        ] {
            let (db, _, trash, id) = setup();
            enqueue(&db, &[id], Op::Delete).unwrap();
            let (mut c, log) = server(move |cmd| Some(match cmd {
                "CAPABILITY" => format!("* CAPABILITY IMAP4rev1{caps}\r\n"),
                "UID SEARCH DELETED" => format!("* SEARCH{deleted}\r\n"),
                FIND => "* SEARCH 9\r\n".into(),
                c if c.starts_with("SELECT ") || c.starts_with("UID COPY ") || c.starts_with("UID STORE ") || c.ends_with("EXPUNGE 5") || c == "EXPUNGE" => String::new(),
                _ => return None,
            }));
            run(&mut c, &db, ME, &Hub::default()).unwrap();
            let want: Vec<&str> = copy.iter().chain(expunge).copied().chain(["SELECT \"Trash\"", FIND]).collect();
            assert_eq!(take(&log), want, "caps{caps:?}");
            assert_eq!(place_of(&db, id), (trash, Some(9)));
        }
    }

    #[test]
    fn missing_on_server_dropped() {
        // moved away and purged on the phone: the gone uid is ignored, Trash has no such message
        let (db, _, _, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => "* SEARCH\r\n".into(),
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert!(db.message(id).unwrap().is_none());
        assert_eq!(states(&db, b), ["done"]);

        // the answer to an earlier MOVE was lost: the uid is gone now, the message is in Trash
        let (db, _, trash, id) = setup();
        enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            "UID MOVE 5 \"Trash\"" => "NO [EXPUNGEISSUED] already gone".into(),
            FIND => "* SEARCH 9\r\n".into(),
            c if c.starts_with("SELECT ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(place_of(&db, id), (trash, Some(9)));

        // its folder is gone from the server
        let (db, _, _, id) = setup();
        let b = enqueue(&db, &[id], Op::Read).unwrap();
        let (mut c, _) = server(|cmd| (cmd == "SELECT \"INBOX\"").then(|| "NO [NONEXISTENT] no such folder".into()));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(states(&db, b), ["done"]);
    }

    #[test]
    fn reject_reverts() {
        let (db, inbox, _, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let r = enqueue(&db, &[id], Op::Read).unwrap();
        let hub = Hub::default();
        let events = hub.subscribe(false);
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            "UID MOVE 5 \"Trash\"" => "NO [OVERQUOTA] Trash is full".into(),
            // the read was enqueued in Trash with no uid yet; after the revert it is not there
            FIND => "* SEARCH\r\n".into(),
            c if c.starts_with("SELECT ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &hub).unwrap();
        assert_eq!(place_of(&db, id), (inbox, Some(5)));
        assert_eq!(ids_in(&db, Role::Inbox), [id]);
        assert_eq!((states(&db, b), states(&db, r)), (vec!["failed".to_string()], vec!["done".to_string()]));
        assert_eq!(events.try_iter().collect::<Vec<_>>(), [format!(r#"{{"event":"action_failed","batch":{b},"error":"[OVERQUOTA] Trash is full"}}"#)]);
        assert!(undo(&db, b).is_err(), "a failed batch has nothing to undo");

        // a refused flag comes back too
        let (db, _, _, id) = setup();
        let r = enqueue(&db, &[id], Op::Read).unwrap();
        let (mut c, _) = server(|cmd| Some(match cmd {
            "SELECT \"INBOX\"" => String::new(),
            "UID STORE 5 +FLAGS.SILENT (\\Seen)" => "NO read-only".into(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &hub).unwrap();
        assert!(!db.message(id).unwrap().unwrap().summary.seen);
        assert_eq!(states(&db, r), ["failed"]);
    }

    #[test]
    fn reject_delete_forever_fetches_again() {
        let (db, _, trash, _) = setup();
        let p = Parsed { subject: "old".into(), message_id: "t3@x".into(), ..Default::default() };
        let id = db.insert_message(trash, 3, true, false, 1, &p, true).unwrap();
        db.set_folder_marks(trash, 3, 3).unwrap();
        let b = enqueue(&db, &[id], Op::DeleteForever).unwrap();
        assert!(ids_in(&db, Role::Trash).is_empty());
        let body = "Subject: old\r\nMessage-ID: <t3@x>\r\n\r\nhi\r\n";
        let (mut c, _) = server(move |cmd| Some(match cmd {
            "SELECT \"Trash\"" => String::new(),
            "UID STORE 3 +FLAGS.SILENT (\\Deleted)" => "NO [CANNOT] not allowed".into(),
            "EXAMINE \"Trash\"" => "* 1 EXISTS\r\n".into(),
            "UID SEARCH UID 3:*" => "* SEARCH 3\r\n".into(),
            "UID FETCH 3 (UID FLAGS RFC822.SIZE)" => "* 1 FETCH (UID 3 FLAGS (\\Seen) RFC822.SIZE 40)\r\n".into(),
            "UID FETCH 3 (UID BODY.PEEK[])" => format!("* 1 FETCH (UID 3 BODY[] {{{}}}\r\n{body})\r\n", body.len()),
            "UID FETCH 3:* (UID FLAGS)" => "* 1 FETCH (UID 3 FLAGS (\\Seen))\r\n".into(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(states(&db, b), ["failed"]);
        let back = db.list(&ListQuery { role: Some(Role::Trash), ..Default::default() }).unwrap();
        assert_eq!(back.iter().map(|s| s.subject.as_str()).collect::<Vec<_>>(), ["old"]);
    }

    #[test]
    fn retry_keeps_pending() {
        let (db, _, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        assert!(has_pending(&db, ME).unwrap());
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            "SELECT \"INBOX\"" => String::new(),
            "UID MOVE 5 \"Trash\"" => "#garbage#\r\n".into(),
            _ => return None,
        }));
        assert!(run(&mut c, &db, ME, &Hub::default()).is_err());
        assert_eq!(states(&db, b), ["pending"]);
        assert_eq!(place_of(&db, id), (trash, None));
        assert!(has_pending(&db, ME).unwrap());
    }

    #[test]
    fn classify_missing() {
        assert_eq!(classify(&Refused("[NONEXISTENT] Unknown Mailbox".into())), Outcome::Missing);
        assert_eq!(classify(&Refused("[EXPUNGEISSUED] Some messages were expunged".into())), Outcome::Missing);
    }

    #[test]
    fn classify_retry() {
        assert_eq!(classify(&Broken("Connection reset by peer (os error 104)".into())), Outcome::Retry);
        assert_eq!(classify(&Broken(format!("{}: Bye Response", imap::UNPARSEABLE))), Outcome::Retry);
        assert_eq!(classify(&Refused("[UNAVAILABLE] try later".into())), Outcome::Retry);
        assert_eq!(classify(&Refused("[inuse] mailbox busy".into())), Outcome::Retry);
    }

    #[test]
    fn classify_reject() {
        assert_eq!(classify(&Refused("[OVERQUOTA] Trash is full".into())), Outcome::Reject);
        assert_eq!(classify(&Refused("Permission denied".into())), Outcome::Reject);
        assert_eq!(classify(&Refused("[NONEXISTENTX] odd".into())), Outcome::Reject);
    }
}
