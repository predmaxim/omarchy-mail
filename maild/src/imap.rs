//! IMAP connection (TLS, TCP keepalive) and the commands the sync needs.

use crate::config::Account;
use crate::db::Role;
use crate::mime::{self, Attachment};
use ::imap::extensions::idle::{SetReadTimeout, WaitOutcome};
use ::imap::types::UnsolicitedResponse;
use imap_proto::{BodyParams, BodyStructure, ContentEncoding, NameAttribute};
use std::io::{Read, Write};
use std::net::{Shutdown, TcpStream, ToSocketAddrs};
use std::os::fd::AsRawFd;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Mutex, PoisonError};
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

impl SetReadTimeout for Stream {
    fn set_read_timeout(&mut self, t: Option<Duration>) -> ::imap::Result<()> {
        match self {
            Stream::Tls(s) => s.get_ref().set_read_timeout(t),
            #[cfg(test)]
            Stream::Plain(s) => TcpStream::set_read_timeout(s, t),
        }
        .map_err(::imap::Error::Io)
    }
}

/// A clone of a connection's socket: `kill` from another thread breaks a blocked read at once.
/// `Conn` keeps it current across `reconnect` and empties it when dropped.
pub type KillSlot = Arc<Mutex<Option<TcpStream>>>;

pub fn kill(slot: &KillSlot) {
    if let Some(s) = &*slot.lock().unwrap_or_else(PoisonError::into_inner) {
        let _ = s.shutdown(Shutdown::Both);
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
    slot: Option<KillSlot>,
    /// INBOX is the selected mailbox (`idle_inbox`).
    inbox: bool,
}

/// Per resolved address.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
/// Socket read/write timeout for the TLS handshake, LOGIN and ordinary commands (not while idling).
const IO_TIMEOUT: Duration = if cfg!(test) { Duration::from_millis(500) } else { Duration::from_secs(60) };

/// Error prefix: the server's answer could not be parsed. The read stopped mid-response, so the
/// session is out of step: `reconnect` before the next command.
pub const UNPARSEABLE: &str = "unparseable server response";
/// Error prefix: the server refused LOGIN (wrong password, account needs attention); retrying
/// on its own will not help.
pub const AUTH_FAILED: &str = "login rejected";

/// Error prefix: the server answered a FETCH with a tagged NO (a message it cannot deliver). The
/// session stays in step, so the next command may follow on the same connection.
pub const REFUSED: &str = "refused by server";

/// A NO about the whole command or the server, not about one message.
fn systemic(code: Option<&imap_proto::ResponseCode>, info: &str) -> bool {
    let info = info.trim_start().to_ascii_uppercase();
    matches!(code, Some(imap_proto::ResponseCode::Alert))
        || ["[ALERT]", "[OVERQUOTA]", "[SERVERBUG]"].iter().any(|c| info.starts_with(c))
}

/// `err`, with a tagged NO about one message told apart. The crate reports a NO that ends a FETCH
/// as an unexpected `Response::Data`.
fn fetch_err(e: ::imap::Error) -> String {
    use imap_proto::{Response, Status};
    match e {
        ::imap::Error::No(ref n) if !systemic(n.code.as_ref(), &n.information) => format!("{REFUSED}: {}", n.information),
        ::imap::Error::Unexpected(Response::Data { status: Status::No, ref code, ref information })
            if !systemic(code.as_ref(), information.as_deref().unwrap_or_default()) =>
            format!("{REFUSED}: {}", information.as_deref().unwrap_or_default()),
        ::imap::Error::Unexpected(Response::Data { status: Status::No, ref information, .. }) =>
            format!("server said NO: {}", information.as_deref().unwrap_or_default()),
        e => err(e),
    }
}

fn err(e: ::imap::Error) -> String {
    match e {
        // The crate reports a line it cannot parse as a BYE.
        ::imap::Error::Bye(_) | ::imap::Error::Parse(_) => format!("{UNPARSEABLE}: {e}"),
        e => e.to_string(),
    }
}

fn io(e: std::io::Error) -> String { e.to_string() }

/// The crate panics on some answers out of protocol (`unreachable!` on an untagged NO inside a
/// SELECT/EXAMINE answer, `assert!`s): such an answer is unparseable like any other.
fn unpanic<T>(f: impl FnOnce() -> ::imap::Result<T>) -> ::imap::Result<T> {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|_| Err(::imap::Error::Parse(::imap::error::ParseError::Invalid(b"panicked".to_vec()))))
}

/// A failed action command (`select`, `command`, `search`, `capabilities`), for `actions::classify`.
#[derive(Debug, Clone, PartialEq)]
pub enum Error {
    /// NO or BAD: the server's text, starting with the response code if the parser did not know it
    /// (`[NONEXISTENT] …`).
    Refused(String),
    /// I/O, a dropped connection, an answer out of protocol (`UNPARSEABLE`): `reconnect` before
    /// the next command.
    Broken(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Refused(s) | Error::Broken(s) => f.write_str(s),
        }
    }
}

impl From<Error> for String {
    fn from(e: Error) -> String { e.to_string() }
}

fn refused(e: ::imap::Error) -> Error {
    match e {
        ::imap::Error::No(n) => Error::Refused(n.information),
        ::imap::Error::Bad(b) => Error::Refused(b.information),
        e => Error::Broken(err(e)),
    }
}

/// An IMAP quoted string; `None` for text that cannot be one (control or non-ASCII characters).
pub fn quote(s: &str) -> Option<String> {
    s.chars().all(|c| c.is_ascii() && !c.is_ascii_control()).then(|| format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\"")))
}

/// The (source uid, new uid) pairs of the first well-formed RFC 4315
/// `[COPYUID <uidvalidity> <source set> <destination set>]` in a `UID MOVE`/`UID COPY` answer.
pub fn parse_copyuid(resp: &str) -> Option<Vec<(u32, u32)>> {
    resp.lines().find_map(|line| {
        let up = line.to_ascii_uppercase();
        let (_, rest) = up.split_once("[COPYUID ")?;
        let (code, _) = rest.split_once(']')?;
        let [validity, src, dst] = code.split_ascii_whitespace().collect::<Vec<_>>()[..] else { return None };
        validity.parse::<u32>().ok().filter(|&v| v > 0)?;
        let (src, dst) = (uid_set(src)?, uid_set(dst)?);
        (src.len() == dst.len()).then(|| src.into_iter().zip(dst).collect())
    })
}

/// `n`, `n:m` (= `m:n`) and comma lists of them, expanded; `None` if malformed or over 100 000 uids.
fn uid_set(s: &str) -> Option<Vec<u32>> {
    let mut out = vec![];
    for part in s.split(',') {
        let (a, b) = part.split_once(':').unwrap_or((part, part));
        let n = |x: &str| x.parse::<u32>().ok().filter(|&n| n > 0);
        let (a, b) = (n(a)?, n(b)?);
        let (lo, hi) = (a.min(b), a.max(b));
        if out.len() as u64 + u64::from(hi - lo) >= 100_000 {
            return None;
        }
        out.extend(lo..=hi);
    }
    Some(out)
}

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

    /// A fresh session to the same server with the same login; the kill slot moves to it.
    pub fn reconnect(&mut self) -> Result<(), String> {
        let mut new = Self::open(self.dial.clone(), &self.login, &self.password)?;
        if let Some(slot) = self.slot.take() {
            new.set_kill_slot(slot);
        }
        *self = new;
        Ok(())
    }

    /// From now on `slot` holds this connection's socket (see `KillSlot`).
    pub fn set_kill_slot(&mut self, slot: KillSlot) {
        *slot.lock().unwrap_or_else(PoisonError::into_inner) = self.tcp.try_clone().ok();
        self.slot = Some(slot);
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
        let session = client.login(login, password).map_err(|(e, _)| match e {
            // An unknown response code stays in the text; UNAVAILABLE is temporary (RFC 5530).
            ::imap::Error::No(no) if !no.information.starts_with("[UNAVAILABLE]") => format!("{AUTH_FAILED}: {no}"),
            e => err(e),
        })?;
        Ok(Conn { session, tcp: killer, dial, login: login.into(), password: password.into(), slot: None, inbox: false })
    }

    /// Syncable folders with their roles, and the Gmail `\All` path (for server-side search).
    pub fn discover(&mut self) -> Result<(Vec<(String, Role)>, Option<String>), String> {
        let names = unpanic(|| self.session.list(Some(""), Some("*"))).map_err(err)?;
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
        self.inbox = false;
        unpanic(|| self.session.examine(path)).map_err(|e| format!("{} (EXAMINE {path})", err(e)))
    }

    /// EXAMINE INBOX (once per session), then IDLE until an update arrives (`true`) or `timeout`
    /// passes (`false`, time to renew). `* OK` keepalive lines are not updates.
    // ponytail: the imap crate drops updates the server sends in its answer to DONE (a round trip
    // per renewal); the next update or the worker's 15-minute SyncAll picks them up. And `timeout`
    // is a read timeout: a server sending `* OK` keepalives more often never gets IDLE renewed
    // (a deadline check in the callback would fix it).
    pub fn idle_inbox(&mut self, timeout: Duration) -> Result<bool, String> {
        if !self.inbox {
            self.examine("INBOX")?;
            self.inbox = true;
        }
        self.session.take_all_unsolicited().for_each(drop);
        let mut idle = self.session.idle();
        idle.timeout(timeout).keepalive(false);
        // The crate panics on some out-of-protocol answers (`unreachable!`, `debug_assert!`).
        let r = catch_unwind(AssertUnwindSafe(|| idle.wait_while(|u| matches!(u, UnsolicitedResponse::Ok { .. }))));
        // The crate leaves the socket with no read timeout at all; DONE (sent on drop) needs one.
        let restored = self.tcp.set_read_timeout(Some(IO_TIMEOUT));
        drop(idle);
        restored.map_err(io)?;
        match r {
            Ok(Ok(WaitOutcome::MailboxChanged)) => Ok(true),
            Ok(Ok(WaitOutcome::TimedOut)) => Ok(false),
            Ok(Err(e)) => Err(err(e)),
            Err(_) => Err(format!("{UNPARSEABLE}: IDLE")),
        }
    }

    /// `UID SEARCH`, ascending.
    pub fn uids(&mut self, query: &str) -> Result<Vec<u32>, String> {
        Ok(self.search(query)?)
    }

    /// `UID SEARCH`, ascending, for actions.
    pub fn search(&mut self, query: &str) -> Result<Vec<u32>, Error> {
        let mut v: Vec<u32> = unpanic(|| self.session.uid_search(query)).map_err(refused)?.into_iter().collect();
        v.sort_unstable();
        Ok(v)
    }

    /// SELECT, read-write: for actions (sync only EXAMINEs).
    pub fn select(&mut self, path: &str) -> Result<::imap::types::Mailbox, Error> {
        if quote(path).is_none() {
            return Err(Error::Refused(format!("{path}: bad folder name")));
        }
        self.session.take_all_unsolicited().for_each(drop);
        self.inbox = false;
        unpanic(|| self.session.select(path)).map_err(refused)
    }

    /// APPENDs a message to `path` flagged `\Seen`.
    pub fn append(&mut self, path: &str, raw: &[u8]) -> Result<(), Error> {
        if quote(path).is_none() {
            return Err(Error::Refused(format!("{path}: bad folder name")));
        }
        unpanic(|| self.session.append(path, raw).flag(::imap::types::Flag::Seen).finish()).map_err(refused).map(drop)
    }

    /// A command that answers just OK or not (`UID MOVE`, `UID STORE … .SILENT`, `EXPUNGE`, …).
    /// Untagged lines in the answer are not looked at.
    pub fn command(&mut self, cmd: &str) -> Result<(), Error> {
        self.command_text(cmd).map(drop)
    }

    /// `command`, with its whole answer, the tagged OK included: where `UID MOVE` and `UID COPY`
    /// tell the new uids (`parse_copyuid`).
    pub fn command_text(&mut self, cmd: &str) -> Result<String, Error> {
        let (data, _) = unpanic(|| self.session.run(cmd)).map_err(refused)?;
        Ok(String::from_utf8_lossy(&data).into_owned())
    }

    /// Capability atoms, upper case (`MOVE`, `UIDPLUS`, …).
    pub fn capabilities(&mut self) -> Result<Vec<String>, Error> {
        let caps = unpanic(|| self.session.capabilities()).map_err(refused)?;
        Ok(caps.iter().filter_map(|c| match c {
            imap_proto::types::Capability::Atom(a) => Some(a.to_ascii_uppercase()),
            _ => None,
        }).collect())
    }

    pub fn fetch(&mut self, uids: &str, query: &str) -> Result<::imap::types::Fetches, String> {
        unpanic(|| self.session.uid_fetch(uids, query)).map_err(fetch_err)
    }

    pub fn noop(&mut self) -> Result<(), String> {
        unpanic(|| self.session.noop()).map_err(err)
    }
}

impl Drop for Conn {
    /// The slot's clone would keep the socket open.
    fn drop(&mut self) {
        if let Some(slot) = &self.slot {
            *slot.lock().unwrap_or_else(PoisonError::into_inner) = None;
        }
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

/// Fake IMAP server for tests, any number of connections: greets, then answers each command with
/// `reply(cmd)` + tagged OK (a reply starting with `NO ` or `OK ` is the tagged answer itself); `None` gets BAD.
/// `IDLE` gets `+ idling` followed by `reply("IDLE")` (untagged updates); `DONE` gets the IDLE's tagged OK.
#[cfg(test)]
pub fn fake_server(reply: impl Fn(&str) -> Option<String> + Send + Sync + 'static) -> std::net::SocketAddr {
    use std::io::{BufRead, BufReader};
    let reply = std::sync::Arc::new(reply);
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    std::thread::spawn(move || {
        for s in l.incoming() {
            let Ok(s) = s else { break };
            let reply = reply.clone();
            std::thread::spawn(move || {
                let mut w = s.try_clone().unwrap();
                let _ = w.write_all(b"* OK fake\r\n");
                let mut idle = String::new();
                for line in BufReader::new(s).lines() {
                    let Ok(line) = line else { break };
                    let out = if line == "DONE" {
                        format!("{idle} OK idle done\r\n")
                    } else {
                        let (tag, cmd) = line.split_once(' ').unwrap_or((&line, ""));
                        match reply(cmd) {
                            Some(r) if cmd == "IDLE" => {
                                idle = tag.into();
                                format!("+ idling\r\n{r}")
                            }
                            Some(r) if r.starts_with("NO ") || r.starts_with("OK ") => format!("{tag} {r}\r\n"),
                            Some(r) => format!("{r}{tag} OK done\r\n"),
                            None => format!("{tag} BAD unexpected {cmd}\r\n"),
                        }
                    };
                    if w.write_all(out.as_bytes()).is_err() { break }
                }
            });
        }
    });
    addr
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

    fn conn(addr: std::net::SocketAddr) -> Result<Conn, String> {
        Conn::plain(TcpStream::connect(addr).unwrap(), "me", "pw")
    }

    #[test]
    fn idle_inbox_update_then_renewal() {
        use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};
        let (examines, idles) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
        let (e, i) = (examines.clone(), idles.clone());
        let mut c = conn(fake_server(move |cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "EXAMINE \"INBOX\"" => { e.fetch_add(1, SeqCst); "* 1 EXISTS\r\n".into() }
            // a keepalive line first: it must not count as an update
            "IDLE" if i.fetch_add(1, SeqCst) == 0 => "* OK still here\r\n* 2 EXISTS\r\n".into(),
            "IDLE" | "NOOP" => String::new(),
            _ => return None,
        }))).unwrap();
        assert_eq!(c.idle_inbox(Duration::from_secs(5)), Ok(true));
        let t = std::time::Instant::now();
        assert_eq!(c.idle_inbox(Duration::from_millis(200)), Ok(false));
        assert!(t.elapsed() >= Duration::from_millis(200));
        assert_eq!(c.tcp.read_timeout().unwrap(), Some(IO_TIMEOUT));
        c.noop().unwrap(); // DONE was answered: the session is in step
        assert_eq!((examines.load(SeqCst), idles.load(SeqCst)), (1, 2));
    }

    #[test]
    fn login_rejection_is_auth_failed() {
        let no = |answer: &'static str| fake_server(move |cmd| cmd.starts_with("LOGIN").then(|| answer.to_string()));
        let e = conn(no("NO [AUTHENTICATIONFAILED] Invalid credentials")).err().unwrap();
        assert!(e.starts_with(AUTH_FAILED), "{e}");
        // temporary (RFC 5530): retried like a network error
        let e = conn(no("NO [UNAVAILABLE] try later")).err().unwrap();
        assert!(!e.starts_with(AUTH_FAILED), "{e}");
    }

    #[test]
    fn tagged_no_is_refused() {
        use imap_proto::{Response, Status};
        let no = ::imap::Error::Unexpected(Response::Data { status: Status::No, code: None, information: Some("[UNAVAILABLE] backend error".into()) });
        assert_eq!(fetch_err(no), format!("{REFUSED}: [UNAVAILABLE] backend error"));
        let ok = ::imap::Error::Unexpected(Response::Data { status: Status::Ok, code: None, information: None });
        assert!(!fetch_err(ok).starts_with(REFUSED));
        for info in ["[ALERT] x", "[OVERQUOTA] x", "[serverbug] x"] {
            let sys = ::imap::Error::Unexpected(Response::Data { status: Status::No, code: None, information: Some(info.into()) });
            assert!(!fetch_err(sys).starts_with(REFUSED), "{info}");
        }
        let io = ::imap::Error::Io(std::io::Error::other("reset"));
        assert!(!fetch_err(io).starts_with(REFUSED));
        let bad = ::imap::Error::Parse(::imap::error::ParseError::Invalid(b"x".to_vec()));
        assert!(fetch_err(bad).starts_with(UNPARSEABLE));
    }

    #[test]
    fn crate_panic_is_unparseable() {
        // the crate hits `unreachable!` on an untagged NO inside an EXAMINE answer
        let mut c = conn(fake_server(|cmd| Some(match cmd {
            c if c.starts_with("LOGIN") => String::new(),
            "EXAMINE \"INBOX\"" => "* NO odd\r\n".into(),
            _ => return None,
        }))).unwrap();
        let e = c.examine("INBOX").unwrap_err();
        assert!(e.starts_with(UNPARSEABLE), "{e}");
    }

    #[test]
    fn copyuid_pairs() {
        assert_eq!(parse_copyuid("* OK [COPYUID 7 5 9] Moved\r\n* 1 EXPUNGE\r\nA3 OK done\r\n"), Some(vec![(5, 9)]));
        assert_eq!(parse_copyuid("A4 ok [copyuid 7 3:5,8 10:12,20] copied\r\n"), Some(vec![(3, 10), (4, 11), (5, 12), (8, 20)]));
        // n:m is the same set as m:n
        assert_eq!(parse_copyuid("* OK [COPYUID 7 5:3 12:10] x"), Some(vec![(3, 10), (4, 11), (5, 12)]));
        // the first well-formed one counts
        assert_eq!(parse_copyuid("* OK [COPYUID x 1 2] a\r\n* OK [COPYUID 7 1 2] b"), Some(vec![(1, 2)]));
        for bad in [
            "A1 OK done", "* OK [COPYUID 7 5] x", "* OK [COPYUID 7 5 9 1] x", "* OK [COPYUID 0 5 9] x", "* OK [COPYUID 7 5 9",
            "* OK [COPYUID 7 1:3 9] x", "* OK [COPYUID 7 * 9] x", "* OK [COPYUID 7 0 9] x", "* OK [COPYUID 7 5, 9] x",
            "* OK [COPYUID 7 a:b 9] x", "* OK [COPYUID 7 1:4294967295 1:4294967295] x", "* OK [COPYUIDS 7 5 9] x", "* OK [COPYUID 7 5\r\n 9] x",
        ] {
            assert_eq!(parse_copyuid(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn kill_slot_follows_reconnect() {
        let mut c = conn(fake_server(|cmd| (cmd.starts_with("LOGIN") || cmd == "NOOP").then(String::new))).unwrap();
        let slot = KillSlot::default();
        c.set_kill_slot(slot.clone());
        c.reconnect().unwrap();
        kill(&slot);
        assert!(c.noop().is_err());
        drop(c);
        assert!(slot.lock().unwrap().is_none());
    }
}
