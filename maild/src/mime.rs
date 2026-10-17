//! Message parsing (via `mail-parser`) and HTML sanitizing. Parsing never panics.

use mail_parser::{HeaderValue, MessageParser, MimeHeaders};
use serde::{Deserialize, Serialize};

const PREVIEW_CHARS: usize = 200;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Addr {
    pub name: String,
    pub addr: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Attachment {
    pub index: u32,
    pub name: String,
    pub mime: String,
    pub size: u64,
    pub cid: Option<String>,
    pub inline: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Parsed {
    /// Without angle brackets.
    pub message_id: String,
    pub in_reply_to: String,
    pub references: Vec<String>,
    pub subject: String,
    pub from: Addr,
    pub to: Vec<Addr>,
    /// Unix seconds; 0 when the message has no valid date.
    pub date: i64,
    pub text: String,
    /// Already sanitized.
    pub html: Option<String>,
    pub preview: String,
    pub attachments: Vec<Attachment>,
}

fn addrs(a: Option<&mail_parser::Address>) -> Vec<Addr> {
    a.map(|a| {
        a.iter()
            .map(|x| Addr {
                name: x.name().unwrap_or("").to_string(),
                addr: x.address().unwrap_or("").to_string(),
            })
            .collect()
    })
    .unwrap_or_default()
}

fn id_list(v: &HeaderValue) -> Vec<String> {
    match v {
        HeaderValue::Text(t) => vec![t.to_string()],
        HeaderValue::TextList(l) => l.iter().map(|t| t.to_string()).collect(),
        _ => vec![],
    }
}

fn attachment_info(index: usize, p: &mail_parser::MessagePart) -> Attachment {
    let mime = match p.content_type() {
        Some(ct) => format!("{}/{}", ct.ctype(), ct.subtype().unwrap_or("octet-stream")).to_lowercase(),
        None => "application/octet-stream".into(),
    };
    let cid = p.content_id().map(|c| c.trim_matches(['<', '>']).to_string());
    let inline = cid.is_some() || p.content_disposition().is_some_and(|d| d.ctype().eq_ignore_ascii_case("inline"));
    Attachment {
        index: index as u32,
        name: p.attachment_name().unwrap_or("attachment").to_string(),
        mime,
        size: p.contents().len() as u64,
        cid,
        inline,
    }
}

pub fn parse(raw: &[u8]) -> Parsed {
    let Some(msg) = MessageParser::default().parse(raw) else {
        return Parsed::default();
    };
    let text = msg.body_text(0).map(|t| t.into_owned()).unwrap_or_default();
    let html = msg
        .html_bodies()
        .find(|p| p.is_text_html())
        .and_then(|p| p.text_contents())
        .map(sanitize_html);
    let from = addrs(msg.from()).into_iter().next().unwrap_or_default();
    Parsed {
        message_id: msg.message_id().unwrap_or("").to_string(),
        in_reply_to: id_list(msg.in_reply_to()).into_iter().next().unwrap_or_default(),
        references: id_list(msg.references()),
        subject: msg.subject().unwrap_or("").to_string(),
        from,
        to: addrs(msg.to()),
        date: msg.date().map(|d| d.to_timestamp()).unwrap_or(0),
        preview: preview(&text, html.as_deref()),
        text,
        html,
        attachments: msg.attachments().enumerate().map(|(i, p)| attachment_info(i, p)).collect(),
    }
}

/// The `index`-th attachment (same numbering as `Parsed::attachments`) with its decoded bytes.
pub fn attachment(raw: &[u8], index: u32) -> Option<(Attachment, Vec<u8>)> {
    let msg = MessageParser::default().parse(raw)?;
    let p = msg.attachments().nth(index as usize)?;
    Some((attachment_info(index as usize, p), p.contents().to_vec()))
}

/// Several attachments (indices as in `Parsed::attachments`) with one parse; missing ones are left out.
pub fn attachments_bytes(raw: &[u8], indices: &[u32]) -> Vec<(u32, Vec<u8>)> {
    let Some(msg) = MessageParser::default().parse(raw) else { return vec![] };
    let all: Vec<_> = msg.attachments().collect();
    indices.iter().filter_map(|&i| Some((i, all.get(i as usize)?.contents().to_vec()))).collect()
}

/// Decodes one text part fetched separately (BODYSTRUCTURE section) by wrapping it in a synthetic header.
pub fn decode_part(bytes: &[u8], content_type: &str, encoding: &str) -> String {
    let mut raw = format!("Content-Type: {content_type}\r\nContent-Transfer-Encoding: {encoding}\r\n\r\n").into_bytes();
    raw.extend_from_slice(bytes);
    MessageParser::default()
        .parse(&raw)
        .and_then(|m| m.root_part().text_contents().map(str::to_string))
        .unwrap_or_default()
}

/// Name of a part known only by its MIME headers (rebuilt from BODYSTRUCTURE); RFC 2231 and encoded words decoded.
pub fn part_name(headers: &str) -> Option<String> {
    let raw = format!("{headers}\r\n");
    MessageParser::default().parse(raw.as_bytes())?.root_part().attachment_name().map(str::to_string)
}

/// Up to 200 chars, whitespace collapsed; falls back to the HTML text.
pub fn preview(text: &str, html: Option<&str>) -> String {
    let src = if text.trim().is_empty() { html.map(strip_tags).unwrap_or_default() } else { text.to_string() };
    let collapsed = src.split_whitespace().collect::<Vec<_>>().join(" ");
    collapsed.chars().take(PREVIEW_CHARS).collect()
}

const DROP_WITH_CONTENT: [&str; 12] = ["script", "iframe", "object", "svg", "math", "textarea", "title", "xmp", "noscript", "noembed", "noframes", "plaintext"];
const DROP_TAG: [&str; 7] = ["embed", "form", "input", "button", "meta", "base", "link"];
const URL_ATTRS: [&str; 5] = ["href", "src", "action", "background", "xlink:href"];

/// End (exclusive) of a non-start tag (end tag, doctype, `<?..>`): the first `>`, like a browser.
fn tag_end(s: &str, i: usize) -> usize {
    s[i..].find('>').map_or(s.len(), |k| i + k + 1)
}

/// Position right after the closing `</name ...>` at or after `from`, or end of input.
fn after_close(lower: &str, from: usize, name: &str) -> usize {
    let pat = format!("</{name}");
    match lower[from..].find(&pat) {
        Some(k) => tag_end(lower, from + k),
        None => lower.len(),
    }
}

/// Decodes character references the way a browser does in an attribute value (one pass).
fn decode_entities(v: &str) -> String {
    let mut out = String::with_capacity(v.len());
    let mut rest = v;
    while let Some(k) = rest.find('&') {
        out.push_str(&rest[..k]);
        rest = &rest[k..];
        let body = &rest[1..];
        let (decoded, used) = if let Some(num) = body.strip_prefix('#') {
            let (digits, radix) = match num.strip_prefix(['x', 'X']) {
                Some(h) => (h, 16),
                None => (num, 10),
            };
            let n = digits.chars().take_while(|c| c.is_digit(radix)).count();
            let ch = u32::from_str_radix(&digits[..n], radix).ok().and_then(char::from_u32);
            let prefix = num.len() - digits.len();
            let semi = digits[n..].starts_with(';') as usize;
            (ch, 1 + 1 + prefix + n + semi)
        } else {
            let named = [("colon;", ':'), ("tab;", '\t'), ("Tab;", '\t'), ("newline;", '\n'), ("NewLine;", '\n'), ("amp;", '&'), ("lt;", '<'), ("gt;", '>'), ("quot;", '"'), ("apos;", '\''), ("nbsp;", '\u{a0}')];
            match named.iter().find(|(n, _)| body.starts_with(n)) {
                Some((n, c)) => (Some(*c), 1 + n.len()),
                None => (None, 0),
            }
        };
        match decoded {
            Some(c) if used > 0 => {
                out.push(c);
                rest = &rest[used..];
            }
            _ => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

fn url_is_unsafe(v: &str) -> bool {
    let c: String = decode_entities(v).chars().filter(|c| !c.is_whitespace() && !c.is_control()).collect::<String>().to_lowercase();
    c.starts_with("javascript:") || c.starts_with("vbscript:") || (c.starts_with("data:") && !c.starts_with("data:image/"))
}

/// Tokenizes the start tag at `s[i]` (name is `name_len` bytes) with HTML-tokenizer rules: quotes matter only
/// right after `=`. Returns the end of the tag and a rebuilt tag holding only safe attributes.
fn start_tag(s: &str, i: usize, name_len: usize) -> (usize, String) {
    let b = s.as_bytes();
    let ws = |c: u8| c.is_ascii_whitespace();
    let mut out = format!("<{}", &s[i + 1..i + 1 + name_len]);
    let mut p = i + 1 + name_len;
    let mut selfclose = false;
    loop {
        while p < b.len() && (ws(b[p]) || b[p] == b'/') {
            selfclose = b[p] == b'/';
            p += 1;
        }
        if p >= b.len() {
            break;
        }
        if b[p] == b'>' {
            p += 1;
            break;
        }
        selfclose = false;
        let st = p;
        if b[p] == b'=' {
            p += 1;
        }
        while p < b.len() && !ws(b[p]) && !matches!(b[p], b'=' | b'/' | b'>') {
            p += 1;
        }
        let name = s[st..p].to_lowercase();
        let mut q = p;
        while q < b.len() && ws(b[q]) {
            q += 1;
        }
        let mut value = "";
        if q < b.len() && b[q] == b'=' {
            q += 1;
            while q < b.len() && ws(b[q]) {
                q += 1;
            }
            if q < b.len() && (b[q] == b'"' || b[q] == b'\'') {
                let quote = b[q];
                let vs = q + 1;
                q = vs;
                while q < b.len() && b[q] != quote {
                    q += 1;
                }
                value = &s[vs..q];
                q = (q + 1).min(b.len());
            } else {
                let vs = q;
                while q < b.len() && !ws(b[q]) && b[q] != b'>' {
                    q += 1;
                }
                value = &s[vs..q];
            }
            p = q;
        }
        if !(name.starts_with("on") || (URL_ATTRS.contains(&name.as_str()) && url_is_unsafe(value))) {
            out.push(' ');
            out.push_str(&s[st..p]);
        }
    }
    if selfclose {
        out.push_str(" /");
    }
    out.push('>');
    (p, out)
}

/// One pass over the tags: drops active content and event/script URLs, keeps `<style>` and everything else.
pub fn sanitize_html(html: &str) -> String {
    let lower = html.to_ascii_lowercase();
    let mut out = String::with_capacity(html.len());
    let mut i = 0;
    while i < html.len() {
        let Some(k) = html[i..].find('<') else {
            out.push_str(&html[i..]);
            break;
        };
        out.push_str(&html[i..i + k]);
        i += k;
        if lower[i..].starts_with("<!--") {
            i = lower[i + 4..].find("-->").map_or(html.len(), |e| i + 4 + e + 3);
            continue;
        }
        let nb = html.as_bytes().get(i + 1).copied().unwrap_or(0);
        if nb == b'!' || nb == b'?' {
            let end = tag_end(html, i);
            out.push_str(&html[i..end]);
            i = end;
            continue;
        }
        let closing = nb == b'/';
        let name_start = i + 1 + closing as usize;
        if !html.as_bytes().get(name_start).is_some_and(u8::is_ascii_alphabetic) {
            out.push('<');
            i += 1;
            continue;
        }
        let name_len = lower[name_start..]
            .find(|c: char| c.is_ascii_whitespace() || c == '/' || c == '>')
            .unwrap_or(html.len() - name_start);
        let name = &lower[name_start..name_start + name_len];
        if closing {
            let end = tag_end(html, i);
            if !DROP_WITH_CONTENT.contains(&name) && !DROP_TAG.contains(&name) {
                out.push_str(&format!("</{}>", &html[name_start..name_start + name_len]));
            }
            i = end;
        } else if DROP_WITH_CONTENT.contains(&name) {
            i = after_close(&lower, start_tag(html, i, name_len).0, name);
        } else if DROP_TAG.contains(&name) {
            i = start_tag(html, i, name_len).0;
        } else if name == "style" {
            // Raw text: a browser ends it only at `</style`, so copy it untokenized and without any `<`.
            let (start, tag) = start_tag(html, i, name_len);
            let stop = lower[start..].find("</style").map_or(html.len(), |k| start + k);
            out.push_str(&tag);
            out.extend(html[start..stop].chars().filter(|&c| c != '<'));
            out.push_str("</style>");
            i = after_close(&lower, stop, "style");
        } else {
            let (end, tag) = start_tag(html, i, name_len);
            out.push_str(&tag);
            i = end;
        }
    }
    out
}

/// Visible text of an HTML fragment (no `<style>`/`<script>` content, common entities decoded).
pub(crate) fn strip_tags(html: &str) -> String {
    let lower = html.to_ascii_lowercase();
    let mut out = String::new();
    let mut i = 0;
    while i < html.len() {
        let Some(k) = html[i..].find('<') else {
            out.push_str(&html[i..]);
            break;
        };
        out.push_str(&html[i..i + k]);
        i += k;
        let end = tag_end(html, i);
        let skipped = ["style", "script"].into_iter().find(|n| lower[i + 1..].starts_with(n));
        i = skipped.map_or(end, |n| after_close(&lower, end, n));
        out.push(' ');
    }
    [("&nbsp;", " "), ("&lt;", "<"), ("&gt;", ">"), ("&quot;", "\""), ("&#39;", "'"), ("&amp;", "&")]
        .iter()
        .fold(out, |s, (a, b)| s.replace(a, b))
}

#[cfg(test)]
mod tests {
    use super::*;

    const PLAIN: &[u8] = include_bytes!("../tests/fixtures/plain.eml");
    const HTML_CID: &[u8] = include_bytes!("../tests/fixtures/html_cid.eml");
    const ATTACH: &[u8] = include_bytes!("../tests/fixtures/attach.eml");
    const CP1251: &[u8] = include_bytes!("../tests/fixtures/cp1251.eml");
    const KOI8: &[u8] = include_bytes!("../tests/fixtures/koi8.eml");
    const BROKEN: &[u8] = include_bytes!("../tests/fixtures/broken.eml");
    const SCRIPT: &[u8] = include_bytes!("../tests/fixtures/script.eml");

    #[test]
    fn plain_fields() {
        let p = parse(PLAIN);
        assert_eq!(p.subject, "Hello plain");
        assert_eq!(p.from.addr, "alice@example.com");
        assert_eq!(p.from.name, "Alice Example");
        assert_eq!(p.to[0].addr, "bob@example.org");
        assert!(p.date > 0);
        assert_eq!(p.message_id, "plain1@example.com");
        assert!(p.html.is_none());
        assert!(p.preview.starts_with("Hello Bob, this is a plain message."));
    }

    #[test]
    fn cid_is_inline() {
        let p = parse(HTML_CID);
        let a = &p.attachments[0];
        assert_eq!(a.cid.as_deref(), Some("logo@x"));
        assert!(a.inline);
        assert!(p.html.unwrap().contains("cid:logo@x"));
    }

    #[test]
    fn attachments_listed() {
        let p = parse(ATTACH);
        assert_eq!(p.attachments.len(), 2);
        assert_eq!(p.attachments[0].name, "doc.pdf");
        assert_eq!(p.attachments[0].mime, "application/pdf");
        assert_eq!(p.attachments[1].name, "pic.png");
        assert_eq!(p.attachments[1].mime, "image/png");
        assert!(!p.attachments[1].inline);
        let (a, bytes) = attachment(ATTACH, 1).unwrap();
        assert_eq!(a.name, "pic.png");
        assert!(bytes.starts_with(b"\x89PNG"));
        assert!(attachment(ATTACH, 5).is_none());
    }

    #[test]
    fn cyrillic_charsets() {
        for raw in [CP1251, KOI8] {
            let p = parse(raw);
            assert_eq!(p.subject, "Привет");
            assert!(p.text.contains("Привет мир"), "{:?}", p.text);
        }
    }

    #[test]
    fn broken_does_not_panic() {
        let p = parse(BROKEN);
        assert_eq!(p.from.addr, "");
        assert_eq!(p.date, 0);
        let _ = parse(b"");
        let _ = parse(&[0xff, 0xfe, b'<', 0x00]);
        for h in ["<", "<a", "<a href=", "<a href='x", "é<é é=é", "<!--", "<script>é", "</", "<p/", "<a b='é"] {
            let _ = sanitize_html(h);
            let _ = preview("", Some(h));
        }
    }

    #[test]
    fn sanitize_strips_active() {
        let html = parse(SCRIPT).html.unwrap();
        for bad in ["<script", "onclick", "onload", "javascript:", "<iframe", "alert(1)"] {
            assert!(!html.contains(bad), "{bad} in {html}");
        }
        assert!(html.contains("<style>p{color:red}</style>"));
        assert!(html.contains(r#"<a href="https://example.com/x">"#));
    }

    #[test]
    fn sanitize_misc() {
        let s = sanitize_html(
            r#"<META HTTP-EQUIV="refresh" content="0;url=x"><img src="data:image/png;base64,AA"><img src="data:text/html,x"><FORM><input><button>b</button></FORM><A HREF=" JavaScript:x">t</A><p ONCLICK=x class=c>"#,
        );
        assert!(!s.to_lowercase().contains("refresh"));
        assert!(s.contains("data:image/png"));
        assert!(!s.contains("data:text"));
        assert!(!s.to_lowercase().contains("<form") && !s.contains("<input") && !s.contains("<button"));
        assert!(!s.to_lowercase().contains("javascript"));
        assert!(!s.to_lowercase().contains("onclick"));
        assert!(s.contains("class=c"));
    }

    #[test]
    fn sanitize_tag_boundaries() {
        for h in [
            r#"<p class=a"b><script>alert(1)</script>""#,
            r#"<!x"><script>alert(1)</script>""#,
            r#"</a "><script>alert(1)</script>""#,
            r#"<p a=b"c><script>alert(1)</script>">"#,
        ] {
            let s = sanitize_html(h).to_lowercase();
            assert!(!s.contains("<script") && !s.contains("alert"), "{h} -> {s}");
        }
    }

    #[test]
    fn sanitize_entity_schemes() {
        for h in [
            r#"<a href="java&#115;cript:alert(1)">x</a>"#,
            r#"<a href="&#x6A;avascript:alert(1)">x</a>"#,
            r#"<a href="javascript&colon;alert(1)">x</a>"#,
            r#"<a href="jav&#x09;ascript:alert(1)">x</a>"#,
            r#"<a href="java&Tab;script:alert(1)">x</a>"#,
            r#"<a href="java&NewLine;script:alert(1)">x</a>"#,
            r#"<a href=&#106avascript:alert(1)>x</a>"#,
        ] {
            let s = sanitize_html(h);
            assert!(!s.contains("href"), "{h} -> {s}");
        }
        assert!(sanitize_html(r#"<a href="https://e.com/?a=1&amp;b=2">x</a>"#).contains("href"));
    }

    #[test]
    fn sanitize_drops_base_link_svg() {
        let s = sanitize_html(r#"<base href="http://e.com/"><link rel=stylesheet href="http://e.com/a.css"><svg onload=x><script>1</script></svg><math><mi>x</mi></math><p>ok</p>"#);
        assert_eq!(s, "<p>ok</p>");
    }

    #[test]
    fn sanitize_raw_text_elements() {
        for h in [
            r#"<style><p title="</style><img src=x onerror=alert(1)>">"#,
            r#"<textarea><p title="</textarea><img src=x onerror=alert(1)>">"#,
            r#"<title><p title="</title><img src=x onerror=alert(1)>">"#,
            r#"<STYLE>a{}</STYLE ><img src=x onerror=alert(1)>"#,
        ] {
            let s = sanitize_html(h).to_lowercase();
            // Any `<img` left is a real tag after the raw-text element, with its handlers stripped.
            assert!(!s.contains("onerror") && !s.contains("alert"), "{h} -> {s}");
        }
        assert_eq!(sanitize_html("<style>a>b{}</style><p>x"), "<style>a>b{}</style><p>x");
    }

    #[test]
    fn decode_part_base64_html() {
        assert_eq!(
            decode_part(b"PGI+0J/RgNC40LLQtdGCPC9iPg==", "text/html; charset=utf-8", "base64"),
            "<b>Привет</b>"
        );
    }

    #[test]
    fn preview_rules() {
        assert_eq!(preview("a \n\n b\t c", None), "a b c");
        assert_eq!(preview(&"я".repeat(300), None).chars().count(), 200);
        assert_eq!(preview("", Some("<p>Hi <b>there</b></p><style>x{}</style>")), "Hi there");
    }
}
