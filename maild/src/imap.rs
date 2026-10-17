//! IMAP connection (TLS, TCP keepalive) and the commands the sync needs.

use crate::config::Account;
use crate::db::Role;
use imap_proto::{BodyStructure, ContentEncoding, NameAttribute};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::fd::AsRawFd;

pub enum Stream {
    Tls(native_tls::TlsStream<TcpStream>),
    #[cfg(test)]
    Plain(TcpStream),
}

impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            Stream::Tls(s) => s.read(buf),
            #[cfg(test)]
            Stream::Plain(s) => s.read(buf),
        }
    }
}

impl Write for Stream {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        match self {
            Stream::Tls(s) => s.write(buf),
            #[cfg(test)]
            Stream::Plain(s) => s.write(buf),
        }
    }
    fn flush(&mut self) -> std::io::Result<()> {
        match self {
            Stream::Tls(s) => s.flush(),
            #[cfg(test)]
            Stream::Plain(s) => s.flush(),
        }
    }
}

pub struct Conn {
    session: ::imap::Session<Stream>,
    tcp: TcpStream,
}

fn err(e: ::imap::Error) -> String { e.to_string() }

/// SO_KEEPALIVE with TCP_KEEPIDLE 60, TCP_KEEPINTVL 10, TCP_KEEPCNT 3: a dead peer is noticed in ~90 s.
fn keepalive(s: &TcpStream) -> Result<(), String> {
    let opts = [
        (libc::SOL_SOCKET, libc::SO_KEEPALIVE, 1),
        (libc::IPPROTO_TCP, libc::TCP_KEEPIDLE, 60),
        (libc::IPPROTO_TCP, libc::TCP_KEEPINTVL, 10),
        (libc::IPPROTO_TCP, libc::TCP_KEEPCNT, 3),
    ];
    for (level, name, value) in opts {
        let v: libc::c_int = value;
        // SAFETY: valid socket fd; the pointer and length describe `v`, which outlives the call.
        let r = unsafe {
            libc::setsockopt(s.as_raw_fd(), level, name, (&v as *const libc::c_int).cast(), size_of::<libc::c_int>() as libc::socklen_t)
        };
        if r != 0 { return Err(format!("setsockopt: {}", std::io::Error::last_os_error())); }
    }
    Ok(())
}

impl Conn {
    pub fn connect(acc: &Account, password: &str) -> Result<Conn, String> {
        let host = acc.imap.host.as_str();
        let tcp = TcpStream::connect((host, acc.imap.port)).map_err(|e| format!("{host}: {e}"))?;
        let tls = native_tls::TlsConnector::new().map_err(|e| e.to_string())?;
        Self::start(tcp, |t| tls.connect(host, t).map(Stream::Tls).map_err(|e| format!("{host}: {e}")), &acc.login, password)
    }

    /// Plaintext session over an already connected stream, for fake servers in tests.
    #[cfg(test)]
    pub fn plain(stream: TcpStream, login: &str, password: &str) -> Result<Conn, String> {
        Self::start(stream, |t| Ok(Stream::Plain(t)), login, password)
    }

    fn start(tcp: TcpStream, wrap: impl FnOnce(TcpStream) -> Result<Stream, String>, login: &str, password: &str) -> Result<Conn, String> {
        keepalive(&tcp)?;
        let killer = tcp.try_clone().map_err(|e| e.to_string())?;
        let mut client = ::imap::Client::new(wrap(tcp)?);
        client.read_greeting().map_err(err)?;
        let session = client.login(login, password).map_err(|(e, _)| err(e))?;
        Ok(Conn { session, tcp: killer })
    }

    /// A handle to the socket: `shutdown` from another thread breaks a blocked read.
    pub fn killer(&self) -> Result<TcpStream, String> {
        self.tcp.try_clone().map_err(|e| e.to_string())
    }

    /// Syncable folders with their roles, and the Gmail `\All` path (for server-side search).
    pub fn discover(&mut self) -> Result<(Vec<(String, Role)>, Option<String>), String> {
        let names = self.session.list(Some(""), Some("*")).map_err(err)?;
        let mut all = None;
        let mut out = vec![];
        for n in names.iter() {
            if n.attributes().contains(&NameAttribute::All) { all = Some(n.name().to_string()); }
            if let Some(role) = role_of(n.name(), n.attributes()) { out.push((n.name().to_string(), role)); }
        }
        Ok((out, all))
    }

    /// Read-only: never changes flags or expunges.
    pub fn examine(&mut self, path: &str) -> Result<::imap::types::Mailbox, String> {
        // Unilateral EXISTS/EXPUNGE pile up for the life of the session; nothing reads them here.
        self.session.take_all_unsolicited().for_each(drop);
        self.session.examine(path).map_err(|e| format!("EXAMINE {path}: {e}"))
    }

    /// `UID SEARCH`, ascending.
    pub fn uids(&mut self, query: &str) -> Result<Vec<u32>, String> {
        let mut v: Vec<u32> = self.session.uid_search(query).map_err(err)?.into_iter().collect();
        v.sort_unstable();
        Ok(v)
    }

    pub fn fetch(&mut self, uids: &str, query: &str) -> Result<::imap::types::Fetches, String> {
        self.session.uid_fetch(uids, query).map_err(err)
    }

    pub fn noop(&mut self) -> Result<(), String> {
        self.session.noop().map_err(err)
    }
}

/// `None` = do not sync (virtual Gmail folders, containers).
pub fn role_of(path: &str, attrs: &[NameAttribute]) -> Option<Role> {
    let skip = attrs.iter().any(|a| match a {
        NameAttribute::All | NameAttribute::Flagged | NameAttribute::NoSelect => true,
        NameAttribute::Extension(x) => x.eq_ignore_ascii_case("\\Important") || x.eq_ignore_ascii_case("\\NonExistent"),
        _ => false,
    });
    if skip { return None; }
    if path.eq_ignore_ascii_case("INBOX") { return Some(Role::Inbox); }
    attrs.iter().find_map(|a| match a {
        NameAttribute::Sent => Some(Role::Sent),
        NameAttribute::Trash => Some(Role::Trash),
        NameAttribute::Junk => Some(Role::Junk),
        _ => None,
    }).or(Some(Role::Other))
}

/// Server data goes into a synthetic header in `mime::decode_part`: keep token characters only.
fn token(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_alphanumeric() || "-_.:".contains(*c)).collect()
}

/// Body text parts (`text/plain`, `text/html`, not attachments): (section, content type with charset, encoding).
pub fn text_sections(bs: &BodyStructure) -> Vec<(String, String, String)> {
    let mut out = vec![];
    walk(bs, "", &mut out);
    out
}

fn walk(bs: &BodyStructure, path: &str, out: &mut Vec<(String, String, String)>) {
    match bs {
        BodyStructure::Multipart { bodies, .. } => {
            for (i, b) in bodies.iter().enumerate() {
                let p = if path.is_empty() { (i + 1).to_string() } else { format!("{path}.{}", i + 1) };
                walk(b, &p, out);
            }
        }
        BodyStructure::Text { common, other, .. } => {
            let sub = common.ty.subtype.to_ascii_lowercase();
            let attached = common.disposition.as_ref().is_some_and(|d| d.ty.eq_ignore_ascii_case("attachment"));
            if !(sub == "plain" || sub == "html") || attached { return; }
            let mut ct = format!("text/{sub}");
            if let Some((_, cs)) = common.ty.params.iter().flatten().find(|(k, _)| k.eq_ignore_ascii_case("charset")) {
                ct += &format!("; charset=\"{}\"", token(cs));
            }
            let enc = match &other.transfer_encoding {
                ContentEncoding::SevenBit => "7bit".into(),
                ContentEncoding::EightBit => "8bit".into(),
                ContentEncoding::Binary => "binary".into(),
                ContentEncoding::Base64 => "base64".into(),
                ContentEncoding::QuotedPrintable => "quoted-printable".into(),
                ContentEncoding::Other(x) => token(x),
            };
            out.push((if path.is_empty() { "1".into() } else { path.into() }, ct, enc));
        }
        // message/rfc822 and non-text parts are attachments
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use imap_proto::{BodyContentCommon, BodyContentSinglePart, ContentDisposition, ContentType};
    use NameAttribute as A;

    fn ext(s: &str) -> A<'_> { A::Extension(s.into()) }

    #[test]
    fn roles_gmail() {
        let none = ext("\\HasNoChildren");
        assert_eq!(role_of("[Gmail]/Вся почта", &[none.clone(), A::All]), None);
        assert_eq!(role_of("[Gmail]/Важное", &[none.clone(), ext("\\Important")]), None);
        assert_eq!(role_of("[Gmail]/Отправленные", &[none.clone(), A::Sent]), Some(Role::Sent));
        assert_eq!(role_of("[Gmail]/Помеченные", &[none.clone(), A::Flagged]), None);
        assert_eq!(role_of("[Gmail]", &[ext("\\HasChildren"), A::NoSelect]), None);
        assert_eq!(role_of("INBOX", &[ext("\\HasNoChildren")]), Some(Role::Inbox));
        assert_eq!(role_of("Inbox", &[]), Some(Role::Inbox));
        assert_eq!(role_of("Работа", &[none]), Some(Role::Other));
    }

    #[test]
    fn roles_yandex() {
        assert_eq!(role_of("Отправленные", &[A::Sent]), Some(Role::Sent));
        assert_eq!(role_of("Спам", &[A::Junk]), Some(Role::Junk));
        assert_eq!(role_of("Удаленные", &[A::Trash]), Some(Role::Trash));
    }

    fn common(ty: &'static str, sub: &'static str, disposition: Option<&'static str>) -> BodyContentCommon<'static> {
        BodyContentCommon {
            ty: ContentType { ty: ty.into(), subtype: sub.into(), params: Some(vec![("CHARSET".into(), "utf-8".into())]) },
            disposition: disposition.map(|d| ContentDisposition { ty: d.into(), params: None }),
            language: None,
            location: None,
        }
    }
    fn single(enc: ContentEncoding<'static>) -> BodyContentSinglePart<'static> {
        BodyContentSinglePart { id: None, md5: None, description: None, transfer_encoding: enc, octets: 10 }
    }
    fn text(sub: &'static str, disposition: Option<&'static str>) -> BodyStructure<'static> {
        BodyStructure::Text { common: common("text", sub, disposition), other: single(ContentEncoding::QuotedPrintable), lines: 1, extension: None }
    }
    fn multi(sub: &'static str, bodies: Vec<BodyStructure<'static>>) -> BodyStructure<'static> {
        BodyStructure::Multipart { common: common("multipart", sub, None), bodies, extension: None }
    }

    #[test]
    fn text_sections_mixed() {
        let pdf = BodyStructure::Basic { common: common("application", "pdf", Some("attachment")), other: single(ContentEncoding::Base64), extension: None };
        let bs = multi("mixed", vec![multi("alternative", vec![text("plain", None), text("html", None)]), pdf, text("plain", Some("attachment"))]);
        let qp = "quoted-printable".to_string();
        assert_eq!(text_sections(&bs), [
            ("1.1".to_string(), "text/plain; charset=\"utf-8\"".to_string(), qp.clone()),
            ("1.2".to_string(), "text/html; charset=\"utf-8\"".to_string(), qp.clone()),
        ]);
        assert_eq!(text_sections(&text("plain", None)), [("1".to_string(), "text/plain; charset=\"utf-8\"".to_string(), qp)]);
    }
}
