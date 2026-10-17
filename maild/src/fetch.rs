//! On-demand mail: bodies of big messages, inline images and attachments, cached under
//! `<cache>/<message id>/` (attachments in `<index>/<name>`, inline images as `cid-<index>`). The only place besides `sync` that downloads whole messages.

use crate::db::{Db, Full};
use crate::i18n::tr;
use crate::imap::Conn;
use crate::mime::{self, Attachment};
use std::collections::HashMap;
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

/// Filesystems limit a name to 255 bytes; room is left for the temporary name.
const NAME_BYTES: usize = 200;
/// Failed inline-image fetches are not repeated more often than this.
pub const CID_RETRY: std::time::Duration = std::time::Duration::from_secs(5 * 60);

pub fn msg_dir(cache: &Path, id: i64) -> PathBuf {
    cache.join(id.to_string())
}

/// The last path component without control characters; nothing left (or `.`/`..`): `attachment-<index>`.
fn safe_name(name: &str, index: u32) -> String {
    let last = name.rsplit(['/', '\\']).next().unwrap_or("");
    let clean: String = last.chars().filter(|c| !c.is_control()).collect();
    match clean.trim() {
        "" | "." | ".." => format!("attachment-{index}"),
        t => cap(t),
    }
}

/// At most `NAME_BYTES` bytes, cut on a char boundary; a short extension is kept.
fn cap(name: &str) -> String {
    if name.len() <= NAME_BYTES {
        return name.into();
    }
    let (stem, ext) = match name.rfind('.') {
        Some(i) if name.len() - i <= 16 => name.split_at(i),
        _ => (name, ""),
    };
    let mut n = NAME_BYTES - ext.len();
    while !stem.is_char_boundary(n) {
        n -= 1;
    }
    format!("{}{}", &stem[..n], ext)
}

/// Where each inline image with a Content-ID is (or will be) stored.
fn cid_map(dir: &Path, atts: &[Attachment]) -> HashMap<String, PathBuf> {
    atts.iter().filter_map(|a| Some((a.cid.clone()?, dir.join(format!("cid-{}", a.index))))).collect()
}

fn wants_cids(f: &Full) -> bool {
    f.html.as_deref().is_some_and(|h| h.to_ascii_lowercase().contains("cid:"))
}

fn is_image(a: &Attachment) -> bool {
    a.cid.is_some() && a.mime.starts_with("image/")
}

/// `cid` → file, for the inline images already cached.
pub fn inline_map(dir: &Path, f: &Full) -> HashMap<String, PathBuf> {
    if !wants_cids(f) {
        return HashMap::new();
    }
    let imgs: Vec<Attachment> = f.attachments.iter().filter(|a| is_image(a)).cloned().collect();
    cid_map(dir, &imgs).into_iter().filter(|(_, p)| p.is_file()).collect()
}

/// The HTML uses inline images that are not cached yet.
pub fn cids_missing(dir: &Path, f: &Full) -> bool {
    wants_cids(f) && f.attachments.iter().filter(|a| is_image(a)).count() > inline_map(dir, f).len()
}

/// The cached file of attachment `index`, if it was downloaded.
pub fn cached_attachment(dir: &Path, f: &Full, index: u32) -> Option<PathBuf> {
    let a = f.attachments.iter().find(|a| a.index == index)?;
    Some(dir.join(index.to_string()).join(safe_name(&a.name, index))).filter(|p| p.is_file())
}

/// Directories 0700, the file 0600; written under a temporary name so a half-written file is never "cached".
fn store(dir: &Path, name: &str, bytes: &[u8]) -> Result<PathBuf, String> {
    let e = |x: std::io::Error| x.to_string();
    std::fs::DirBuilder::new().recursive(true).mode(0o700).create(dir).map_err(e)?;
    let path = dir.join(name);
    let tmp = dir.join(format!(".{name}.part"));
    let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp).map_err(e)?;
    f.write_all(bytes).and_then(|_| f.sync_all()).map_err(e)?;
    std::fs::rename(&tmp, &path).map_err(e)?;
    Ok(path)
}

/// The whole message from the server. A message whose move is pending has no UID yet.
fn raw(conn: &mut Conn, db: &Db, id: i64) -> Result<(Full, Vec<u8>), String> {
    let full = db.message(id)?.ok_or_else(|| tr("message not found", "письмо не найдено"))?;
    let uid = full.uid.ok_or_else(|| tr("message is being moved, try again", "письмо перемещается, попробуйте ещё раз"))?;
    let folder = db.folders_of(&full.summary.account)?.into_iter().find(|f| f.id == full.folder)
        .ok_or_else(|| tr("no such folder", "нет такой папки"))?;
    let mb = conn.examine(&folder.path)?;
    if folder.uidvalidity != 0 && mb.uid_validity.is_some_and(|v| v != folder.uidvalidity) {
        return Err(tr("the folder changed on the server, sync and try again", "папка изменилась на сервере, обновите и повторите"));
    }
    let got = conn.fetch(&uid.to_string(), "(UID BODY.PEEK[])")?;
    let body = got.iter().find(|f| f.uid == Some(uid)).and_then(|f| f.body())
        .ok_or_else(|| tr("message is gone from the server", "письма уже нет на сервере"))?;
    Ok((full, body.to_vec()))
}

/// Job::Fetch: stores the body of a big message and caches its inline images. Stored attachments
/// are replaced with mail-parser's list, the numbering `attachment` uses (a big message's came
/// from BODYSTRUCTURE).
/// `true` if it stored the body or cached an image.
pub fn fetch_body(conn: &mut Conn, db: &Db, cache: &Path, id: i64) -> Result<bool, String> {
    let (full, raw) = raw(conn, db, id)?;
    let p = mime::parse(&raw);
    let mut changed = !full.has_body;
    if !full.has_body {
        db.set_body(id, &p.text, p.html.as_deref(), &p.preview)?;
    }
    db.set_attachments(id, &p.attachments)?;
    let dir = msg_dir(cache, id);
    let want: Vec<u32> = p.attachments.iter().filter(|a| is_image(a)).map(|a| a.index).filter(|i| !dir.join(format!("cid-{i}")).is_file()).collect();
    for (index, bytes) in mime::attachments_bytes(&raw, &want) {
        store(&dir, &format!("cid-{index}"), &bytes)?;
        changed = true;
    }
    Ok(changed)
}

/// Job::Attachment. The requested one is found in the parsed message by (name, mime) of the stored
/// entry: the stored index may come from BODYSTRUCTURE and differ from mail-parser's.
pub fn fetch_attachment(conn: &mut Conn, db: &Db, cache: &Path, id: i64, index: u32) -> Result<PathBuf, String> {
    let (full, raw) = raw(conn, db, id)?;
    let wanted = full.attachments.iter().find(|a| a.index == index)
        .ok_or_else(|| tr("no such attachment", "нет такого вложения"))?;
    let p = mime::parse(&raw);
    let same = |a: &&Attachment| a.name == wanted.name && a.mime == wanted.mime;
    let at = p.attachments.iter().filter(same).find(|a| a.index == index).or_else(|| p.attachments.iter().find(same)).map_or(index, |a| a.index);
    db.set_attachments(id, &p.attachments)?;
    let (_, bytes) = mime::attachment(&raw, at).ok_or_else(|| tr("no such attachment", "нет такого вложения"))?;
    store(&msg_dir(cache, id).join(at.to_string()), &safe_name(&wanted.name, at), &bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mime::Attachment;

    #[test]
    fn long_cyrillic_name_fits() {
        let name = format!("{}.pdf", "Договор ".repeat(19));
        assert!(name.chars().count() > 150 && name.len() > 255);
        let n = safe_name(&name, 0);
        assert!(n.len() <= NAME_BYTES && n.ends_with(".pdf") && n.starts_with("Договор"), "{n}");
        assert!(safe_name(&"я".repeat(150), 0).len() <= NAME_BYTES);
    }

    fn att(index: u32, name: &str, mime: &str, cid: Option<&str>) -> Attachment {
        Attachment { index, name: name.into(), mime: mime.into(), size: 1, cid: cid.map(Into::into), inline: cid.is_some() }
    }

    #[test]
    fn attachment_name_sanitized() {
        assert_eq!(safe_name("../../etc/passwd", 0), "passwd");
        assert_eq!(safe_name("..\\..\\win.ini", 1), "win.ini");
        assert_eq!(safe_name("", 3), "attachment-3");
        assert_eq!(safe_name("..", 4), "attachment-4");
        assert_eq!(safe_name("a/", 5), "attachment-5");
        assert_eq!(safe_name("сч\u{7}ёт.pdf", 0), "счёт.pdf");
    }

    #[test]
    fn cid_paths_map() {
        let dir = Path::new("/c/7");
        let atts = [att(0, "a.pdf", "application/pdf", None), att(1, "l.png", "image/png", Some("logo@x")), att(2, "b.png", "image/png", Some("b@x"))];
        let m = cid_map(dir, &atts);
        assert_eq!(m.len(), 2);
        assert_eq!(m["logo@x"], PathBuf::from("/c/7/cid-1"));
        assert_eq!(m["b@x"], PathBuf::from("/c/7/cid-2"));
    }
}
