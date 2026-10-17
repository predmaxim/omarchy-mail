//! IMAP connection (TLS, TCP keepalive) and the commands the sync needs.

use crate::config::Account;
use crate::db::Role;
use crate::mime::{self, Attachment};
use imap_proto::{BodyParams, BodyStructure, ContentEncoding, NameAttribute};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::os::fd::AsRawFd;
use std::time::Duration;

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

#[derive(Clone)]
enum Dial {
    Tls(String, u16),
    #[cfg(test)]
    Plain(std::net::SocketAddr),
}

pub struct Conn {
    session: ::imap::Session<Stream>,
    tcp: TcpStream,
    dial: Dial,
    login: String,
    password: String,
}

/// Per resolved address.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(30);
/// Socket read/write timeout for the TLS handshake, LOGIN and ordinary commands.
/// IDLE (task 5) must set its own longer read timeout while idling and restore this one after.
const IO_TIMEOUT: Duration = if cfg!(test) { Duration::from_millis(500) } else { Duration::from_secs(60) };

/// Error prefix: the server's answer could not be parsed. The read stopped mid-response, so the
/// session is out of step: `reconnect` before the next command.
pub const UNPARSEABLE: &str = "unparseable server response";

fn err(e: ::imap::Error) -> String {
    match e {
        // The crate reports a line it cannot parse as a BYE.
        ::imap::Error::Bye(_) | ::imap::Error::Parse(_) => format!("{UNPARSEABLE}: {e}"),
        e => e.to_string(),
    }
}

fn io(e: std::io::Error) -> String { e.to_string() }

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

fn tcp_connect(host: &str, port: u16) -> Result<TcpStream, String> {
    let mut last = format!("{host}: no address");
    for addr in (host, port).to_socket_addrs().map_err(|e| format!("{host}: {e}"))? {
        match TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT) {
            Ok(s) => return Ok(s),
            Err(e) => last = format!("{host}: {e}"),
        }
    }
    Err(last)
}

impl Conn {
    pub fn connect(acc: &Account, password: &str) -> Result<Conn, String> {
        Self::open(Dial::Tls(acc.imap.host.clone(), acc.imap.port), &acc.login, password)
    }

    /// Plaintext session over an already connected stream, for fake servers in tests.
    #[cfg(test)]
    pub fn plain(stream: TcpStream, login: &str, password: &str) -> Result<Conn, String> {
        let addr = stream.peer_addr().map_err(io)?;
        Self::start(stream, Dial::Plain(addr), login, password)
    }

    /// A fresh session to the same server with the same login.
    pub fn reconnect(&mut self) -> Result<(), String> {
        *self = Self::open(self.dial.clone(), &self.login, &self.password)?;
        Ok(())
    }

    fn open(dial: Dial, login: &str, password: &str) -> Result<Conn, String> {
        let tcp = match &dial {
            Dial::Tls(host, port) => tcp_connect(host, *port)?,
            #[cfg(test)]
            Dial::Plain(addr) => TcpStream::connect_timeout(addr, CONNECT_TIMEOUT).map_err(io)?,
        };
        Self::start(tcp, dial, login, password)
    }

    fn start(tcp: TcpStream, dial: Dial, login: &str, password: &str) -> Result<Conn, String> {
        keepalive(&tcp)?;
        tcp.set_read_timeout(Some(IO_TIMEOUT)).map_err(io)?;
        tcp.set_write_timeout(Some(IO_TIMEOUT)).map_err(io)?;
        let killer = tcp.try_clone().map_err(io)?;
        let stream = match &dial {
            Dial::Tls(host, _) => {
                let tls = native_tls::TlsConnector::new().map_err(|e| e.to_string())?;
                Stream::Tls(tls.connect(host, tcp).map_err(|e| format!("{host}: {e}"))?)
            }
            #[cfg(test)]
            Dial::Plain(_) => Stream::Plain(tcp),
        };
        let mut client = ::imap::Client::new(stream);
        client.read_greeting().map_err(err)?;
        let session = client.login(login, password).map_err(|(e, _)| err(e))?;
        Ok(Conn { session, tcp: killer, dial, login: login.into(), password: password.into() })
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
        self.session.examine(path).map_err(|e| format!("{} (EXAMINE {path})", err(e)))
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

/// Header-safe bare value (type/subtype, parameter name).
fn bare(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_graphic() && !"\";\\".contains(*c)).collect()
}

/// `; k="v"` for each BODYSTRUCTURE parameter, values escaped for a quoted string.
fn params(ps: &BodyParams) -> String {
    let mut out = String::new();
    for (k, v) in ps.iter().flatten() {
        out += &format!("; {}=\"", bare(k));
        for c in v.chars().filter(|c| !c.is_control()) {
            if c == '"' || c == '\\' { out.push('\\'); }
            out.push(c);
        }
        out.push('"');
    }
    out
}

/// Leaf parts depth-first with their section numbers; message/rfc822 is one leaf.
fn leaves<'b, 'a>(bs: &'b BodyStructure<'a>, path: &str, out: &mut Vec<(String, &'b BodyStructure<'a>)>) {
    if let BodyStructure::Multipart { bodies, .. } = bs {
        for (i, b) in bodies.iter().enumerate() {
            leaves(b, &if path.is_empty() { (i + 1).to_string() } else { format!("{path}.{}", i + 1) }, out);
        }
    } else {
        out.push((if path.is_empty() { "1".into() } else { path.into() }, bs));
    }
}

fn all_leaves<'b, 'a>(bs: &'b BodyStructure<'a>) -> Vec<(String, &'b BodyStructure<'a>)> {
    let mut out = vec![];
    leaves(bs, "", &mut out);
    out
}

/// Body text parts (`text/plain`, `text/html`, not attachments): (section, content type with charset, encoding).
pub fn text_sections(bs: &BodyStructure) -> Vec<(String, String, String)> {
    all_leaves(bs).into_iter().filter_map(|(section, b)| {
        let BodyStructure::Text { common, other, .. } = b else { return None };
        let sub = common.ty.subtype.to_ascii_lowercase();
        let attached = common.disposition.as_ref().is_some_and(|d| d.ty.eq_ignore_ascii_case("attachment"));
        if !(sub == "plain" || sub == "html") || attached { return None; }
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
        Some((section, ct, enc))
    }).collect()
}

/// Every leaf except the `body` sections, depth-first; `index` is the position in that order.
/// Names are decoded (RFC 2231, encoded words) by rebuilding the part's MIME headers for mail-parser.
pub fn attachments(bs: &BodyStructure, body: &[&str]) -> Vec<Attachment> {
    let parts = all_leaves(bs).into_iter().filter(|(section, _)| !body.contains(&section.as_str())).filter_map(|(_, b)| match b {
        BodyStructure::Basic { common, other, .. } | BodyStructure::Text { common, other, .. } | BodyStructure::Message { common, other, .. } => Some((common, other)),
        BodyStructure::Multipart { .. } => None,
    });
    parts.enumerate().map(|(i, (common, other))| {
        let ty = format!("{}/{}", common.ty.ty, common.ty.subtype).to_lowercase();
        let disposition = common.disposition.as_ref();
        let mut headers = format!("Content-Type: {}{}\r\n", bare(&ty), params(&common.ty.params));
        if let Some(d) = disposition {
            headers += &format!("Content-Disposition: {}{}\r\n", bare(&d.ty), params(&d.params));
        }
        let cid = other.id.as_ref().map(|c| c.trim().trim_matches(['<', '>']).to_string()).filter(|c| !c.is_empty());
        Attachment {
            index: i as u32,
            name: mime::part_name(&headers).unwrap_or_else(|| "attachment".into()),
            mime: ty,
            size: other.octets.into(),
            inline: cid.is_some() || disposition.is_some_and(|d| d.ty.eq_ignore_ascii_case("inline")),
            cid,
        }
    }).collect()
}

/// RFC 3501 modified UTF-7 (`&…-` = base64 of UTF-16BE with `,` for `/`; `&-` = `&`).
/// Malformed input comes back unchanged.
pub fn decode_mutf7(s: &str) -> String {
    try_mutf7(s).unwrap_or_else(|| s.to_string())
}

fn try_mutf7(s: &str) -> Option<String> {
    let mut out = String::new();
    let mut rest = s;
    while let Some(i) = rest.find('&') {
        out.push_str(&rest[..i]);
        let (b64, tail) = rest[i + 1..].split_once('-')?;
        rest = tail;
        if b64.is_empty() {
            out.push('&');
            continue;
        }
        let (mut acc, mut bits, mut bytes) = (0u32, 0, vec![]);
        for c in b64.bytes() {
            let v = match c {
                b'A'..=b'Z' => c - b'A',
                b'a'..=b'z' => c - b'a' + 26,
                b'0'..=b'9' => c - b'0' + 52,
                b'+' => 62,
                b',' => 63,
                _ => return None,
            };
            acc = (acc << 6 | u32::from(v)) & 0xffff;
            bits += 6;
            if bits >= 8 {
                bits -= 8;
                bytes.push((acc >> bits) as u8);
            }
        }
        if bytes.len() % 2 != 0 { return None; }
        let units = bytes.chunks(2).map(|p| u16::from_be_bytes([p[0], p[1]]));
        for ch in char::decode_utf16(units) { out.push(ch.ok()?); }
    }
    out.push_str(rest);
    Some(out)
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

    #[test]
    fn attachments_from_structure() {
        let mut c = common("image", "png", None);
        c.ty.params = Some(vec![("name*".into(), "utf-8''%D1%84.png".into())]);
        let img = BodyStructure::Basic { common: c, other: BodyContentSinglePart { id: Some("<img1@x>".into()), ..single(ContentEncoding::Base64) }, extension: None };
        let a = attachments(&multi("related", vec![text("html", None), img]), &["1"]);
        let got: Vec<_> = a.iter().map(|a| (a.index, a.name.as_str(), a.mime.as_str(), a.size, a.cid.as_deref(), a.inline)).collect();
        assert_eq!(got, [(0, "ф.png", "image/png", 10, Some("img1@x"), true)]);
    }

    #[test]
    fn mutf7_names() {
        assert_eq!(decode_mutf7("&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-"), "Отправленные");
        assert_eq!(decode_mutf7("[Gmail]/&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-"), "[Gmail]/Отправленные");
        assert_eq!(decode_mutf7("Work&-Life"), "Work&Life");
        assert_eq!(decode_mutf7("INBOX"), "INBOX");
        for bad in ["&BB4", "&!!-", "&BB-", "a&", "&2D3-"] {
            assert_eq!(decode_mutf7(bad), bad);
        }
    }

    #[test]
    fn silent_server_times_out() {
        // Never accepted: the kernel completes the TCP handshake, nobody sends the greeting.
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let t = std::time::Instant::now();
        assert!(Conn::plain(TcpStream::connect(l.local_addr().unwrap()).unwrap(), "me", "pw").is_err());
        assert!(t.elapsed() < std::time::Duration::from_secs(3));
    }
}
