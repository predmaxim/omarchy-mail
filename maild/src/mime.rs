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

/// Decodes one text part fetched separately (BODYSTRUCTURE section) by wrapping it in a synthetic header.
pub fn decode_part(bytes: &[u8], content_type: &str, encoding: &str) -> String {
    let mut raw = format!("Content-Type: {content_type}\r\nContent-Transfer-Encoding: {encoding}\r\n\r\n").into_bytes();
    raw.extend_from_slice(bytes);
    MessageParser::default()
        .parse(&raw)
        .and_then(|m| m.root_part().text_contents().map(str::to_string))
        .unwrap_or_default()
}

/// Up to 200 chars, whitespace collapsed; falls back to the HTML text.
pub fn preview(text: &str, html: Option<&str>) -> String {
    let src = if text.trim().is_empty() { html.map(strip_tags).unwrap_or_default() } else { text.to_string() };
    let collapsed = src.split_whitespace().collect::<Vec<_>>().join(" ");
    collapsed.chars().take(PREVIEW_CHARS).collect()
}

const DROP_WITH_CONTENT: [&str; 3] = ["script", "iframe", "object"];
const DROP_TAG: [&str; 5] = ["embed", "form", "input", "button", "meta"];
const URL_ATTRS: [&str; 5] = ["href", "src", "action", "background", "xlink:href"];

/// End (exclusive) of the tag starting at `i` (`s[i] == '<'`), honouring quoted attribute values.
fn tag_end(s: &str, i: usize) -> usize {
    let mut quote = None;
    for (k, c) in s[i..].char_indices() {
        match (quote, c) {
            (None, '>') => return i + k + 1,
            (None, '"' | '\'') => quote = Some(c),
            (Some(q), c) if c == q => quote = None,
            _ => {}
        }
    }
    s.len()
}

/// Position right after the closing `</name ...>` at or after `from`, or end of input.
fn after_close(lower: &str, from: usize, name: &str) -> usize {
    let pat = format!("</{name}");
    match lower[from..].find(&pat) {
        Some(k) => tag_end(lower, from + k),
        None => lower.len(),
    }
}

fn url_is_unsafe(v: &str) -> bool {
    let c: String = v.chars().filter(|c| !c.is_whitespace() && !c.is_control()).collect::<String>().to_lowercase();
    c.starts_with("javascript:") || c.starts_with("vbscript:") || (c.starts_with("data:") && !c.starts_with("data:image/"))
}

/// Rebuilds a tag keeping only safe attributes. `inner` is the text after `<`, without the closing `>`.
fn clean_tag(inner: &str, name_end: usize) -> String {
    let mut out = String::from("<");
    out.push_str(&inner[..name_end]);
    let rest = &inner[name_end..];
    let b = rest.as_bytes();
    let mut i = 0;
    let mut selfclose = false;
    while i < b.len() {
        if b[i].is_ascii_whitespace() || b[i] == b'/' {
            selfclose = b[i] == b'/';
            i += 1;
            continue;
        }
        selfclose = false;
        let start = i;
        while i < b.len() && !b[i].is_ascii_whitespace() && b[i] != b'=' && b[i] != b'/' {
            i += 1;
        }
        let name = rest[start..i].to_lowercase();
        let mut j = i;
        while j < b.len() && b[j].is_ascii_whitespace() {
            j += 1;
        }
        let mut value = "";
        if j < b.len() && b[j] == b'=' {
            j += 1;
            while j < b.len() && b[j].is_ascii_whitespace() {
                j += 1;
            }
            let vs = j;
            if j < b.len() && (b[j] == b'"' || b[j] == b'\'') {
                let q = b[j];
                j += 1;
                while j < b.len() && b[j] != q {
                    j += 1;
                }
                value = &rest[vs + 1..j.min(b.len())];
                j = (j + 1).min(b.len());
            } else {
                while j < b.len() && !b[j].is_ascii_whitespace() {
                    j += 1;
                }
                value = &rest[vs..j];
            }
            i = j;
        }
        let bad = name.starts_with("on") || (URL_ATTRS.contains(&name.as_str()) && url_is_unsafe(value));
        if !bad {
            out.push(' ');
            out.push_str(&rest[start..i]);
        }
    }
    if selfclose {
        out.push_str(" /");
    }
    out.push('>');
    out
}

/// One pass over the tags: drops active content and event/script URLs, keeps `<style>` and everything else.
// ponytail: HTML-entity-obfuscated schemes (`java&#115;cript:`) are not decoded; add if the viewer ever resolves them.
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
        if !(nb.is_ascii_alphabetic() || nb == b'/' || nb == b'!' || nb == b'?') {
            out.push('<');
            i += 1;
            continue;
        }
        let end = tag_end(html, i);
        let closing = nb == b'/';
        let name_start = i + 1 + closing as usize;
        let name_len = lower[name_start..end]
            .find(|c: char| c.is_ascii_whitespace() || c == '/' || c == '>')
            .unwrap_or(end - name_start);
        let name = &lower[name_start..name_start + name_len];
        if DROP_WITH_CONTENT.contains(&name) {
            i = if closing { end } else { after_close(&lower, end, name) };
        } else if DROP_TAG.contains(&name) {
            i = end;
        } else if nb == b'!' || nb == b'?' || closing {
            out.push_str(&html[i..end]);
            i = end;
        } else {
            let inner = html[i + 1..end].strip_suffix('>').unwrap_or(&html[i + 1..end]);
            out.push_str(&clean_tag(inner, name_len));
            i = end;
        }
    }
    out
}

/// Visible text of an HTML fragment (no `<style>`/`<script>` content, common entities decoded).
fn strip_tags(html: &str) -> String {
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
