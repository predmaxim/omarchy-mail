use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Provider {
    Gmail,
    Yandex,
    Imap,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Server {
    pub host: String,
    pub port: u16,
}

/// `email` is the mailbox key everywhere.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Account {
    pub email: String,
    pub name: String,
    pub provider: Provider,
    pub login: String,
    pub imap: Server,
    pub smtp: Server,
    pub append_sent: bool,
}

#[derive(Serialize, Deserialize)]
struct File {
    #[serde(default)]
    account: Vec<Account>,
}

fn server(host: &str, port: u16) -> Server {
    Server { host: host.into(), port }
}

/// (imap, smtp, append_sent)
pub fn preset(p: Provider) -> Option<(Server, Server, bool)> {
    match p {
        Provider::Gmail => Some((server("imap.gmail.com", 993), server("smtp.gmail.com", 465), false)),
        // Checked live 2026-10-17: Yandex does not keep a copy of mail sent over SMTP.
        Provider::Yandex => Some((server("imap.yandex.ru", 993), server("smtp.yandex.ru", 465), true)),
        Provider::Imap => None,
    }
}

#[derive(Debug, Clone)]
pub struct Paths {
    pub config: PathBuf,
    pub db: PathBuf,
    pub cache: PathBuf,
    pub socket: PathBuf,
    /// Clickable notifications, kept across a maild restart (see `notify`).
    pub notify_ids: PathBuf,
}

pub fn paths() -> Paths {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/".into());
    // SAFETY: getuid has no preconditions.
    paths_from(|k| std::env::var(k).ok(), &home, unsafe { libc::getuid() })
}

fn paths_from(get: impl Fn(&str) -> Option<String>, home: &str, uid: u32) -> Paths {
    let dir = |var: &str, fallback: &str| {
        get(var).filter(|v| !v.is_empty()).map(PathBuf::from).unwrap_or_else(|| Path::new(home).join(fallback))
    };
    let runtime = get("XDG_RUNTIME_DIR")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{uid}")));
    Paths {
        config: dir("XDG_CONFIG_HOME", ".config").join("omarchy-mail/accounts.toml"),
        db: dir("XDG_DATA_HOME", ".local/share").join("omarchy-mail/mail.db"),
        cache: dir("XDG_CACHE_HOME", ".cache").join("omarchy-mail"),
        socket: runtime.join("omarchy-mail.sock"),
        notify_ids: runtime.join("omarchy-mail/notify-ids.json"),
    }
}

pub fn load(path: &Path) -> Result<Vec<Account>, String> {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(vec![]),
        Err(e) => return Err(format!("{}: {e}", path.display())),
    };
    let f: File = toml::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(f.account)
}

/// Atomic: temp file + rename.
pub fn save(path: &Path, accounts: &[Account]) -> Result<(), String> {
    let text = toml::to_string_pretty(&File { account: accounts.to_vec() }).map_err(|e| e.to_string())?;
    let err = |e: std::io::Error| format!("{}: {e}", path.display());
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(err)?;
    }
    let tmp = path.with_extension("toml.tmp");
    std::fs::write(&tmp, text).map_err(err)?;
    std::fs::rename(&tmp, path).map_err(err)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn acc(email: &str) -> Account {
        let (imap, smtp, append_sent) = preset(Provider::Gmail).unwrap();
        Account { email: email.into(), name: "N".into(), provider: Provider::Gmail, login: email.into(), imap, smtp, append_sent }
    }

    fn tmp(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("maild-test-{}-{name}", std::process::id()))
    }

    #[test]
    fn preset_gmail() {
        let (i, s, a) = preset(Provider::Gmail).unwrap();
        assert_eq!((i.host.as_str(), i.port), ("imap.gmail.com", 993));
        assert_eq!((s.host.as_str(), s.port), ("smtp.gmail.com", 465));
        assert!(!a);
        assert!(preset(Provider::Imap).is_none());
        // Yandex does not keep SMTP-sent mail, so maild files the copy itself.
        assert!(preset(Provider::Yandex).unwrap().2);
    }

    #[test]
    fn roundtrip_accounts() {
        let p = tmp("rt").join("sub/accounts.toml");
        let v = vec![acc("a@example.invalid"), acc("b@example.invalid")];
        save(&p, &v).unwrap();
        assert_eq!(load(&p).unwrap(), v);
        std::fs::remove_dir_all(tmp("rt")).unwrap();
    }

    #[test]
    fn load_missing_is_empty() {
        assert_eq!(load(&tmp("missing")).unwrap(), vec![]);
    }

    #[test]
    fn paths_follow_xdg() {
        let p = paths_from(|k| (k == "XDG_RUNTIME_DIR").then(|| "/r".into()), "/h", 1000);
        assert_eq!(p.socket, PathBuf::from("/r/omarchy-mail.sock"));
        assert_eq!(p.notify_ids, PathBuf::from("/r/omarchy-mail/notify-ids.json"));
        assert_eq!(p.config, PathBuf::from("/h/.config/omarchy-mail/accounts.toml"));
        assert_eq!(p.db, PathBuf::from("/h/.local/share/omarchy-mail/mail.db"));
        assert_eq!(p.cache, PathBuf::from("/h/.cache/omarchy-mail"));
        let q = paths_from(|_| None, "/h", 1000);
        assert_eq!(q.socket, PathBuf::from("/run/user/1000/omarchy-mail.sock"));
    }
}
