//! What the user does to mail (delete, read, flag, …). The database changes at once; the server
//! follows from a queue that survives restarts: `actions` rows, `pending` → `done` | `failed`.
//! Each row records where its message was when it was enqueued, so the worker replays the local
//! history on the server in `id` order, also after a time offline.

use crate::account::Job;
use crate::db::{Db, Role};
use crate::hub::{Event, Hub};
use crate::i18n::tr;
use crate::imap::{self, Conn};
use crate::mime;
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

/// Failed tries on the connection before an action counts as refused.
const MAX_TRIES: u32 = 5;

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
            "SELECT m.folder, m.uid, m.message_id, m.seen, m.flagged, f.account, f.role, COALESCE(f.uidvalidity, 0)
             FROM messages m JOIN folders f ON f.id = m.folder WHERE m.id = ?1",
            [id],
            |r| Ok((r.get::<_, i64>(0)?, r.get::<_, Option<u32>>(1)?, r.get::<_, String>(2)?, r.get::<_, bool>(3)?, r.get::<_, bool>(4)?, r.get::<_, String>(5)?, r.get::<_, String>(6)?, r.get::<_, u32>(7)?)),
        ).optional().map_err(sql)?;
        // Gone meanwhile: a sync removed it.
        let Some((folder, uid, message_id, seen, flagged, account, role, validity)) = msg else { continue };
        if role == Role::Remote.as_str() {
            return Err(tr("open from search results: actions are not available", "открыто из результатов поиска: действия недоступны"));
        }
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
            "INSERT INTO actions(batch, account, op, message, from_folder, to_folder, uid, uidvalidity, message_id, state)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 'pending')",
            params![batch, account, op.name(), id, folder, to, uid, validity, message_id],
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
/// `uid` is valid under `validity` (the source folder's UIDVALIDITY then, 0 = unknown); `moved` is
/// set once the server has confirmed the MOVE.
struct Pending {
    id: i64, batch: i64, op: Op, message: i64, from: i64, to: Option<i64>,
    from_path: Option<String>, to_path: Option<String>, uid: Option<u32>, validity: u32, message_id: String, moved: bool,
}

/// The worker: replays the account's pending actions on the server, in order. A connection failure
/// ends it with `Err`; that action and the ones after it stay pending for the next `Actions`.
pub fn run(conn: &mut Conn, db: &Db, account: &str, hub: &Hub) -> Result<(), String> {
    let pending = rows(db,
        "SELECT a.id, a.batch, a.op, a.message, a.from_folder, a.to_folder, ff.path, tf.path, a.uid, a.message_id, COALESCE(a.uidvalidity, 0) FROM actions a
         LEFT JOIN folders ff ON ff.id = a.from_folder LEFT JOIN folders tf ON tf.id = a.to_folder
         WHERE a.account = ?1 AND a.state = 'pending' ORDER BY a.id",
        [account],
        |r| {
            let Some(op) = Op::parse(r.get(2)?) else { return Ok(None) };
            Ok(Some(Pending {
                id: r.get(0)?, batch: r.get(1)?, op, message: r.get(3)?, from: r.get(4)?, to: r.get(5)?, from_path: r.get(6)?,
                to_path: r.get(7)?, uid: r.get(8)?, message_id: r.get::<_, Option<String>>(9)?.unwrap_or_default(), validity: r.get(10)?,
                moved: false,
            }))
        })?;
    let mut s = Server { conn, db, account, selected: None, caps: None };
    let mut told = None;
    for mut a in pending.into_iter().flatten() {
        let found = match execute(&mut s, &mut a) {
            Ok(found) => found,
            Err(e) => match classify(&e) {
                Outcome::Missing => None,
                outcome => {
                    let broken = outcome == Outcome::Retry;
                    // One that keeps failing on the connection must not hold up the ones after it.
                    if broken && add_try(db, a.id)? < MAX_TRIES {
                        return Err(format!("{}: {e}", a.op.name()));
                    }
                    if broken {
                        // The session is out of step.
                        s.conn.reconnect()?;
                        s.selected = None;
                    }
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
        // Not found where it went: that folder's sync brings it back, sooner than the periodic one.
        if settle(db, &a, found)? && let Some(to) = a.to && let Some(h) = hub.handles().into_iter().find(|h| h.email == account) {
            let _ = h.jobs.send(Job::SyncFolder(to));
        }
    }
    Ok(())
}

/// Counts a failed try of action `id`; returns how many there have been.
fn add_try(db: &Db, id: i64) -> Result<u32, String> {
    db.c.query_row("UPDATE actions SET tries = tries + 1 WHERE id = ?1 RETURNING tries", [id], |r| r.get(0)).map_err(sql)
}

/// The server side of one action; fills in `a.uid` when it was unknown or stale, and `a.moved`.
/// `Ok(uid)`: where a move's message is now in its target folder (`None` if not found there);
/// `None` for the other actions.
fn execute(s: &mut Server, a: &mut Pending) -> Result<Option<u32>, imap::Error> {
    let Some(from) = &a.from_path else { return Ok(None) };
    let validity = s.select(from)?;
    // The folder was recreated: the stored uid may name another message now.
    if a.validity != 0 && validity.is_some_and(|v| v != a.validity) {
        a.uid = None;
    }
    // No uid: enqueued while an earlier move of the message was still on its way.
    if a.uid.is_none() {
        a.uid = s.find(&a.message_id)?;
    }
    let store = match a.op {
        Op::Delete | Op::Restore => {
            let Some(to) = &a.to_path else { return Ok(None) };
            // A gone uid: moved by an earlier try whose answer was lost, or by a phone.
            match a.uid.map(|u| s.mv(u, to)) {
                Some(Ok(new)) => {
                    a.moved = true;
                    if new.is_some() {
                        return Ok(new);
                    }
                }
                Some(Err(e)) if classify(&e) != Outcome::Missing => return Err(e),
                _ => {}
            }
            // Moved, or already there: a refusal from here on is not the move's.
            return match s.select(to).and_then(|_| s.find(&a.message_id)) {
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
/// its uid there. Not found there: if the server did not move it, it goes back to its source with
/// its uid (whose sync drops it if it is gone there too); otherwise it goes, and `true` says the
/// target's sync will bring it.
fn settle(db: &Db, a: &Pending, found: Option<u32>) -> Result<bool, String> {
    let tx = begin(db)?;
    let mut lost = false;
    if let Some(to) = a.to && awaiting(&tx, a.message, to)? {
        match found {
            Some(uid) => place(&tx, a.message, to, Some(uid))?,
            None if !a.moved && a.uid.is_some() && a.from_path.is_some() => place(&tx, a.message, a.from, a.uid)?,
            None => {
                tx.execute("DELETE FROM messages WHERE id = ?1", [a.message]).map_err(sql)?;
                lost = true;
            }
        }
    }
    tx.execute("UPDATE actions SET state = 'done' WHERE id = ?1", [a.id]).map_err(sql)?;
    tx.commit().map_err(sql)?;
    Ok(lost)
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

/// The worker's connection during one `run`: what is selected (with its UIDVALIDITY and UIDNEXT),
/// what the server can do.
struct Server<'a> { conn: &'a mut Conn, db: &'a Db, account: &'a str, selected: Option<(String, Option<u32>, Option<u32>)>, caps: Option<(bool, bool)> }

/// How many of the newest uids `find` scans when the server cannot search headers.
const SCAN: u32 = 200;
/// How many uids one scan FETCH names.
const CHUNK: usize = 20;

impl Server<'_> {
    /// SELECT, unless already selected; the folder's UIDVALIDITY if the server told it.
    fn select(&mut self, path: &str) -> Result<Option<u32>, imap::Error> {
        if let Some((p, v, _)) = &self.selected && p == path {
            return Ok(*v);
        }
        self.selected = None;
        let m = self.conn.select(path)?;
        self.selected = Some((path.into(), m.uid_validity, m.uid_next));
        Ok(m.uid_validity)
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

    /// The newest message with this Message-ID in the selected folder. A server that refuses the
    /// search (Yandex: `[UNAVAILABLE] UID SEARCH Backend error`, every time) gets the newest
    /// messages scanned instead; only if that fails too is it the search's error.
    fn find(&mut self, message_id: &str) -> Result<Option<u32>, imap::Error> {
        let Some(q) = imap::quote(&format!("<{message_id}>")).filter(|_| !message_id.is_empty()) else { return Ok(None) };
        match self.conn.search(&format!("HEADER Message-ID {q}")) {
            Err(imap::Error::Refused(e)) => self.scan(message_id).map_err(|s| imap::Error::Broken(format!("{e}; scanning instead: {s}"))),
            r => Ok(r?.last().copied()),
        }
    }

    /// `find` without header SEARCH: the Message-IDs of the newest `SCAN` uids, compared here, `CHUNK`
    /// uids per FETCH, newest first; older chunks are not fetched once one matches. Uids in the
    /// folder's `refused` table are left out, and a FETCH the server refuses is redone one uid at
    /// a time, skipping the refused ones (Yandex: a broken message fails the whole FETCH).
    // ponytail: a message further down counts as not found (its folder's sync brings it back);
    // a wider window if servers that cannot search are common.
    fn scan(&mut self, message_id: &str) -> Result<Option<u32>, String> {
        let (path, next) = self.selected.as_ref().map_or(("", None), |s| (s.0.as_str(), s.2));
        let from = next.map_or(1, |n| n.saturating_sub(SCAN).max(1));
        let skip = match self.db.folders_of(self.account)?.iter().find(|f| f.path == path) {
            Some(f) => self.db.refused_uids(f.id)?,
            None => vec![],
        };
        // UID SEARCH UID works where SEARCH HEADER does not; without it every uid of the window is named.
        let mut uids = match self.conn.search(&format!("UID {from}:*")) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("{}: UID SEARCH UID {from}:* failed: {e}", path);
                (from..next.unwrap_or(from)).collect()
            }
        };
        uids.retain(|u| !skip.contains(u));
        uids.sort_unstable_by(|a, b| b.cmp(a));
        uids.truncate(SCAN as usize);
        let norm = |id: &str| id.trim().trim_start_matches('<').trim_end_matches('>').trim().to_ascii_lowercase();
        let want = norm(message_id);
        for chunk in uids.chunks(CHUNK) {
            let mut one = |us: &[u32]| -> Result<Option<u32>, String> {
                let set = us.iter().map(u32::to_string).collect::<Vec<_>>().join(",");
                let got = self.conn.fetch(&set, "(UID BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])")?;
                Ok(got.iter().filter(|f| f.header().is_some_and(|h| norm(&mime::parse(h).message_id) == want)).filter_map(|f| f.uid).max())
            };
            match one(chunk) {
                Ok(Some(u)) => return Ok(Some(u)),
                Ok(None) => {}
                Err(e) if e.starts_with(imap::REFUSED) => {
                    for u in chunk {
                        match one(&[*u]) {
                            Ok(Some(u)) => return Ok(Some(u)),
                            Ok(None) => {}
                            Err(e) if e.starts_with(imap::REFUSED) => eprintln!("{}: uid {u} refused by server: {e}", path),
                            Err(e) => return Err(e),
                        }
                    }
                }
                Err(e) => return Err(e),
            }
        }
        Ok(None)
    }

    /// `UID MOVE`; without MOVE: copy, mark \Deleted, expunge. The message's new uid if the server
    /// told it (UIDPLUS COPYUID).
    fn mv(&mut self, uid: u32, to: &str) -> Result<Option<u32>, imap::Error> {
        let q = imap::quote(to).ok_or_else(|| imap::Error::Refused(format!("{to}: bad folder name")))?;
        let new = |answer: String| imap::parse_copyuid(&answer)?.into_iter().find(|p| p.0 == uid).map(|p| p.1);
        if self.caps()?.0 {
            return self.conn.command_text(&format!("UID MOVE {uid} {q}")).map(new);
        }
        // ponytail: a connection lost between COPY and EXPUNGE copies it again on the retry (a duplicate, nothing lost).
        let copied = new(self.conn.command_text(&format!("UID COPY {uid} {q}"))?);
        self.conn.command(&format!("UID STORE {uid} +FLAGS.SILENT (\\Deleted)"))?;
        self.expunge(uid)?;
        Ok(copied)
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
        eprintln!("{}: uid {uid} stays marked \\Deleted: no UIDPLUS, and other messages are marked too", self.selected.as_ref().map_or("", |(p, _, _)| p));
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

    #[test]
    fn remote_message_rejects_actions() {
        let (db, _, _, _) = setup();
        let f = db.upsert_folder(ME, "@remote/INBOX", Role::Remote).unwrap();
        let id = db.insert_message(f, 9, false, false, 1, &Parsed::default(), true).unwrap();
        assert!(enqueue(&db, &[id], Op::Read).is_err());
        assert!(!db.message(id).unwrap().unwrap().summary.seen, "changed locally");
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

    /// Moves uid 5 INBOX → Trash, where it is 9, and 9 back to INBOX, where it is 11.
    fn mover() -> (Conn, Log) {
        let selected = Mutex::new(String::new());
        server(move |cmd| {
            if let Some(path) = cmd.strip_prefix("SELECT ") {
                *selected.lock().unwrap() = path.into();
                return Some(String::new());
            }
            Some(match cmd {
                "CAPABILITY" => CAPS.into(),
                "UID MOVE 5 \"Trash\"" | "UID MOVE 9 \"INBOX\"" => String::new(),
                FIND if *selected.lock().unwrap() == "\"Trash\"" => "* SEARCH 9\r\n".into(),
                FIND => "* SEARCH 11\r\n".into(),
                _ => return None,
            })
        })
    }

    #[test]
    fn move_and_restore_on_server() {
        let (db, inbox, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, log) = mover();
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
    }

    #[test]
    fn restored_mail_is_not_new_mail() {
        let (db, inbox, _, id) = setup();
        db.set_folder_marks(inbox, 5, 5).unwrap();
        let (mut c, _) = mover();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        undo(&db, b).unwrap();
        // Mail 10 came while offline; the restore lands above it, as 11, before the Inbox sync.
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        // What that sync then stores.
        let new = db.insert_message(inbox, 10, false, false, 1, &Parsed { subject: "new".into(), ..Default::default() }, true).unwrap();
        let p = Parsed { subject: "s".into(), message_id: "m1@x".into(), ..Default::default() };
        assert_eq!(db.insert_message(inbox, 11, false, false, 1, &p, true).unwrap(), id);
        let notified = db.folders_of(ME).unwrap()[0].notified_uid;
        db.set_folder_marks(inbox, 11, notified).unwrap();
        let unread: Vec<i64> = db.unread_inbox(ME).unwrap().into_iter().map(|s| s.id).collect();
        assert_eq!(unread, [new], "the new mail is announced, the restored one is not");
    }

    #[test]
    fn stale_uid_after_uidvalidity_change() {
        for (validity, store) in [(7, "UID STORE 5 +FLAGS.SILENT (\\Seen)"), (8, "UID STORE 12 +FLAGS.SILENT (\\Seen)")] {
            let db = Db::memory();
            let inbox = db.upsert_folder(ME, "INBOX", Role::Inbox).unwrap();
            db.reset_folder(inbox, 7).unwrap();
            let p = Parsed { message_id: "m1@x".into(), ..Default::default() };
            let id = db.insert_message(inbox, 5, false, false, 1, &p, true).unwrap();
            enqueue(&db, &[id], Op::Read).unwrap();
            let (mut c, log) = server(move |cmd| Some(match cmd {
                "SELECT \"INBOX\"" => format!("* OK [UIDVALIDITY {validity}] v\r\n"),
                // recreated folder: uid 5 may be another message now, this one is 12
                FIND => "* SEARCH 12\r\n".into(),
                c if c == store => String::new(),
                _ => return None,
            }));
            run(&mut c, &db, ME, &Hub::default()).unwrap();
            let want: Vec<&str> = if validity == 7 { vec!["SELECT \"INBOX\"", store] } else { vec!["SELECT \"INBOX\"", FIND, store] };
            assert_eq!(take(&log), want);
        }
    }

    #[test]
    fn poison_action_gives_up() {
        let (db, inbox, _, id) = setup();
        let other = db.insert_message(inbox, 6, false, false, 1, &Parsed { message_id: "m2@x".into(), ..Default::default() }, true).unwrap();
        let del = enqueue(&db, &[id], Op::Delete).unwrap();
        let read = enqueue(&db, &[other], Op::Read).unwrap();
        let hub = Hub::default();
        let events = hub.subscribe(false);
        let (mut c, log) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            // every try of this one breaks the session
            "UID MOVE 5 \"Trash\"" => "#garbage#\r\n".into(),
            c if c.starts_with("SELECT ") || c == "UID STORE 6 +FLAGS.SILENT (\\Seen)" => String::new(),
            _ => return None,
        }));
        for _ in 1..MAX_TRIES {
            assert!(run(&mut c, &db, ME, &hub).is_err());
            assert_eq!((states(&db, del), states(&db, read)), (vec!["pending".to_string()], vec!["pending".to_string()]));
            c.reconnect().unwrap(); // as the worker's `attempt` does
        }
        run(&mut c, &db, ME, &hub).unwrap();
        assert_eq!((states(&db, del), states(&db, read)), (vec!["failed".to_string()], vec!["done".to_string()]));
        assert_eq!(place_of(&db, id), (inbox, Some(5)));
        assert!(take(&log).iter().any(|c| c == "UID STORE 6 +FLAGS.SILENT (\\Seen)"));
        assert_eq!(events.try_iter().filter(|e| e.contains("action_failed")).count(), 1);
    }

    #[test]
    fn failed_move_keeps_mail() {
        // Trash is gone on the server: nothing moved, so the message goes back where it was.
        let (db, inbox, _, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            "SELECT \"INBOX\"" => String::new(),
            "UID MOVE 5 \"Trash\"" | "SELECT \"Trash\"" => "NO [NONEXISTENT] no such folder".into(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(place_of(&db, id), (inbox, Some(5)));
        assert_eq!(states(&db, b), ["done"]);
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
    fn copyuid_skips_search() {
        let (db, inbox, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, log) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            "UID MOVE 5 \"Trash\"" => "* OK [COPYUID 7 5 9] Moved\r\n* 1 EXPUNGE\r\n".into(),
            "UID MOVE 9 \"INBOX\"" => "* OK [COPYUID 3 9 11] Moved\r\n* 1 EXPUNGE\r\n".into(),
            c if c.starts_with("SELECT ") => String::new(),
            _ => return None,
        }));
        let hub = Hub::default();
        run(&mut c, &db, ME, &hub).unwrap();
        assert_eq!(take(&log), ["SELECT \"INBOX\"", "CAPABILITY", "UID MOVE 5 \"Trash\""]);
        assert_eq!(place_of(&db, id), (trash, Some(9)));
        assert_eq!(states(&db, b), ["done"]);
        let u = undo(&db, b).unwrap();
        run(&mut c, &db, ME, &hub).unwrap();
        assert_eq!(take(&log), ["SELECT \"Trash\"", "CAPABILITY", "UID MOVE 9 \"INBOX\""]);
        assert_eq!(place_of(&db, id), (inbox, Some(11)));
        assert_eq!(states(&db, u), ["done"]);
    }

    #[test]
    fn copy_fallback_takes_copyuid() {
        let (db, _, trash, id) = setup();
        enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, log) = server(|cmd| Some(match cmd {
            "CAPABILITY" => "* CAPABILITY IMAP4rev1 UIDPLUS\r\n".into(),
            // in the tagged OK for COPY (RFC 4315)
            "UID COPY 5 \"Trash\"" => "OK [COPYUID 7 5 9] Copied".into(),
            c if c.starts_with("SELECT ") || c.starts_with("UID STORE ") || c == "UID EXPUNGE 5" => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(take(&log), ["SELECT \"INBOX\"", "CAPABILITY", "UID COPY 5 \"Trash\"", "UID STORE 5 +FLAGS.SILENT (\\Deleted)", "UID EXPUNGE 5"]);
        assert_eq!(place_of(&db, id), (trash, Some(9)));
    }

    const HDR: &str = "(UID BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)])";
    const SEARCH_ALL: &str = "UID SEARCH UID 1:*";
    const TRASH_NEXT: &str = "* OK [UIDNEXT 13] x\r\n";
    /// The scan's fetch of these uids, newest first.
    fn scan_cmd(uids: &[u32]) -> String {
        format!("UID FETCH {} {HDR}", uids.iter().map(u32::to_string).collect::<Vec<_>>().join(","))
    }
    /// The uids named by a scan's `UID FETCH`, None for any other command.
    fn fetched(cmd: &str) -> Option<Vec<u32>> {
        let set = cmd.strip_prefix("UID FETCH ")?.strip_suffix(HDR)?.trim();
        Some(set.split(',').filter_map(|u| u.parse().ok()).collect())
    }
    const BACKEND: &str = "NO [UNAVAILABLE] UID SEARCH Backend error. sc=UXT6170ke4Y0_";

    /// `* n FETCH` lines with each (uid, Message-ID header line).
    fn ids_answer(msgs: &[(u32, &str)]) -> String {
        msgs.iter().enumerate().map(|(i, (uid, h))| {
            let h = format!("{h}\r\n\r\n");
            format!("* {} FETCH (UID {uid} BODY[HEADER.FIELDS (MESSAGE-ID)] {{{}}}\r\n{h})\r\n", i + 1, h.len())
        }).collect()
    }

    #[test]
    fn search_backend_error_scans() {
        let (db, _, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, log) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => BACKEND.into(),
            "SELECT \"Trash\"" => TRASH_NEXT.into(),
            SEARCH_ALL => "* SEARCH 7 9 10 12\r\n".into(),
            c if c == scan_cmd(&[12, 10, 9, 7]) => ids_answer(&[(7, "Message-ID: <m1@x>"), (9, "Message-ID:\r\n  <M1@X> "), (10, "Message-ID: <m2@x>"), (12, "Subject: no id")]),
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert_eq!(take(&log), ["SELECT \"INBOX\"", "CAPABILITY", "UID MOVE 5 \"Trash\"", "SELECT \"Trash\"", FIND, SEARCH_ALL, &scan_cmd(&[12, 10, 9, 7])]);
        assert_eq!(place_of(&db, id), (trash, Some(9)), "the newest match");
        assert_eq!(states(&db, b), ["done"]);
        assert_eq!(db.c.query_row("SELECT tries FROM actions WHERE batch = ?1", [b], |r| r.get::<_, u32>(0)).unwrap(), 0);
    }

    #[test]
    fn search_backend_error_scan_misses() {
        // moved, but neither the search nor the scan finds it: Trash's sync brings it back
        let (db, _, _, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => BACKEND.into(),
            "SELECT \"Trash\"" => TRASH_NEXT.into(),
            SEARCH_ALL => "* SEARCH 8\r\n".into(),
            c if c == scan_cmd(&[8]) => ids_answer(&[(8, "Message-ID: <m2@x>")]),
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        assert!(db.message(id).unwrap().is_none());
        assert_eq!(states(&db, b), ["done"]);

        // the scan fails too: a try, as before
        let (db, _, trash, id) = setup();
        let b = enqueue(&db, &[id], Op::Delete).unwrap();
        let (mut c, _) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => BACKEND.into(),
            "SELECT \"Trash\"" => TRASH_NEXT.into(),
            SEARCH_ALL => "* SEARCH 8\r\n".into(),
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        let e = run(&mut c, &db, ME, &Hub::default()).unwrap_err();
        assert!(e.contains("Backend error"), "{e}");
        assert_eq!(states(&db, b), ["pending"]);
        assert_eq!(place_of(&db, id), (trash, None));
        assert_eq!(db.c.query_row("SELECT tries FROM actions WHERE batch = ?1", [b], |r| r.get::<_, u32>(0)).unwrap(), 1);
    }

    /// Trash window 1..=100 (UIDNEXT 101); the message is `want`, `broken` uids are refused by the
    /// server when fetched; (db, message, trash folder, Conn, log).
    fn window(want: u32, broken: &'static [u32]) -> (Db, i64, i64, Conn, Log) {
        let (db, _, trash, id) = setup();
        enqueue(&db, &[id], Op::Delete).unwrap();
        let all = (1..=100).map(|u| u.to_string()).collect::<Vec<_>>().join(" ");
        let (c, log) = server(move |cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => BACKEND.into(),
            "SELECT \"Trash\"" => "* OK [UIDNEXT 101] x\r\n".into(),
            "UID SEARCH UID 1:*" => format!("* SEARCH {all}\r\n"),
            c if fetched(c).is_some() => {
                let us = fetched(c).unwrap_or_default();
                if us.iter().any(|u| broken.contains(u)) {
                    return Some("NO [UNAVAILABLE] failure due to backend error".into());
                }
                let h: Vec<String> = us.iter().map(|u| format!("Message-ID: <{}@x>", if *u == want { "m1".into() } else { format!("o{u}") })).collect();
                ids_answer(&us.iter().zip(&h).map(|(u, h)| (*u, h.as_str())).collect::<Vec<_>>())
            }
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        (db, id, trash, c, log)
    }

    fn fetches(log: &Log) -> Vec<Vec<u32>> {
        take(log).iter().filter_map(|c| fetched(c)).collect()
    }

    #[test]
    fn scan_stops_at_first_chunk_and_skips_refused_table() {
        let (db, id, trash, mut c, log) = window(85, &[]);
        let t = db.upsert_folder(ME, "Trash", Role::Trash).unwrap();
        db.add_refused(t, 100).unwrap();
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        // 100 is never fetched; the match is in the first chunk, so no older one is
        assert_eq!(fetches(&log), [(80..=99).rev().collect::<Vec<u32>>()]);
        assert_eq!(place_of(&db, id), (trash, Some(85)));
    }

    #[test]
    fn scan_refused_chunk_goes_one_by_one() {
        // 95 is refused, in the newest chunk; the message is in the next one
        let (db, id, trash, mut c, log) = window(70, &[95]);
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        let mut want: Vec<Vec<u32>> = vec![(81..=100).rev().collect()];
        want.extend((81..=100).rev().map(|u| vec![u]));
        want.push((61..=80).rev().collect());
        assert_eq!(fetches(&log), want);
        assert_eq!(place_of(&db, id), (trash, Some(70)));
    }

    #[test]
    fn scan_without_uid_search_walks_ranges() {
        let (db, id, trash, _, _) = window(95, &[]);
        let (mut c, log) = server(|cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => BACKEND.into(),
            "SELECT \"Trash\"" => "* OK [UIDNEXT 101] x\r\n".into(),
            "UID SEARCH UID 1:*" => "NO [UNAVAILABLE] Backend error".into(),
            c if let Some(us) = fetched(c) => {
                if us.contains(&99) {
                    return Some("NO [UNAVAILABLE] failure due to backend error".into());
                }
                let h: Vec<String> = us.iter().map(|u| format!("Message-ID: <{}@x>", if *u == 95 { "m1".into() } else { format!("o{u}") })).collect();
                ids_answer(&us.iter().zip(&h).map(|(u, h)| (*u, h.as_str())).collect::<Vec<_>>())
            }
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        let mut want: Vec<Vec<u32>> = vec![(81..=100).rev().collect()];
        want.extend([100, 99, 98, 97, 96, 95].map(|u| vec![u]));
        assert_eq!(fetches(&log), want);
        assert_eq!(place_of(&db, id), (trash, Some(95)));
    }

    #[test]
    fn scan_without_uidnext_looks_at_newest_200() {
        let (db, id, trash, _, _) = window(900, &[]);
        let all = (1..=1000).map(|u| u.to_string()).collect::<Vec<_>>().join(" ");
        let (mut c, log) = server(move |cmd| Some(match cmd {
            "CAPABILITY" => CAPS.into(),
            FIND => BACKEND.into(),
            "UID SEARCH UID 1:*" => format!("* SEARCH {all}\r\n"),
            c if let Some(us) = fetched(c) => {
                let h: Vec<String> = us.iter().map(|u| format!("Message-ID: <{}@x>", if *u == 900 { "m1".into() } else { format!("o{u}") })).collect();
                ids_answer(&us.iter().zip(&h).map(|(u, h)| (*u, h.as_str())).collect::<Vec<_>>())
            }
            c if c.starts_with("SELECT ") || c.starts_with("UID MOVE ") => String::new(),
            _ => return None,
        }));
        run(&mut c, &db, ME, &Hub::default()).unwrap();
        let f = fetches(&log);
        assert_eq!(f[0], (981..=1000).rev().collect::<Vec<u32>>(), "newest first");
        assert!(f.concat().iter().all(|u| *u > 800), "only the newest 200");
        assert_eq!(place_of(&db, id), (trash, Some(900)));
    }

    #[test]
    fn yandex_recovery() {
        // The live state: the delete's MOVE went through, its SEARCH failed 4 times; then undo.
        // The uid is gone from INBOX, which the server may say either way.
        for gone in ["NO [NONEXISTENT] No such message", ""] {
            let db = Db::memory();
            let inbox = db.upsert_folder(ME, "INBOX", Role::Inbox).unwrap();
            db.upsert_folder(ME, "Trash", Role::Trash).unwrap();
            let p = Parsed { subject: "s".into(), message_id: "m1@x".into(), ..Default::default() };
            let id = db.insert_message(inbox, 12541, false, false, 1, &p, true).unwrap();
            let del = enqueue(&db, &[id], Op::Delete).unwrap();
            db.c.execute("UPDATE actions SET tries = 4 WHERE batch = ?1", [del]).unwrap();
            let res = undo(&db, del).unwrap();
            let (search, scan, one) = ("UID SEARCH UID 2900:*", scan_cmd(&[3051, 3050, 3049]), scan_cmd(&[3051]));
            let (scan2, one2) = (scan.clone(), one.clone());
            let (mut c, log) = server(move |cmd| Some(match cmd {
                "CAPABILITY" => CAPS.into(),
                "SELECT \"INBOX\"" => "* OK [UIDNEXT 12600] x\r\n".into(),
                "SELECT \"Trash\"" => "* OK [UIDNEXT 3100] x\r\n".into(),
                "UID MOVE 12541 \"Trash\"" => gone.into(),
                FIND => BACKEND.into(),
                c if c == search => "* SEARCH 3049 3050 3051\r\n".into(),
                // 3051 is a message the server will not deliver
                c if c == scan2 || c == one2 => "NO [UNAVAILABLE] UID FETCH message 5 uid 3051: failure due to backend error".into(),
                c if c == scan_cmd(&[3050]) => ids_answer(&[(3050, "Message-ID: <m1@x>")]),
                "UID MOVE 3050 \"INBOX\"" => "* OK [COPYUID 1 3050 12600] Moved\r\n* 5 EXPUNGE\r\n".into(),
                _ => return None,
            }));
            let hub = Hub::default();
            let events = hub.subscribe(false);
            run(&mut c, &db, ME, &hub).unwrap();
            assert_eq!(take(&log), [
                "SELECT \"INBOX\"", "CAPABILITY", "UID MOVE 12541 \"Trash\"", "SELECT \"Trash\"", FIND, search, &scan, &one, &scan_cmd(&[3050]),
                FIND, search, &scan, &one, &scan_cmd(&[3050]), "UID MOVE 3050 \"INBOX\"",
            ], "{gone:?}");
            assert_eq!(place_of(&db, id), (inbox, Some(12600)));
            assert_eq!((states(&db, del), states(&db, res)), (vec!["done".to_string()], vec!["done".to_string()]));
            assert_eq!(db.queued_count(ME).unwrap(), 0);
            assert!(events.try_iter().all(|e| !e.contains("action_failed")));
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
