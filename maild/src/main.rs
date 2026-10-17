mod account;
mod actions;
mod config;
mod db;
mod fetch;
mod hub;
mod i18n;
mod imap;
mod ipc;
mod mime;
mod notify;
mod search;
mod secret;
mod smtp;
mod sync;
mod wake;

use serde_json::{Value, json};
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::UnixStream;

fn usage() -> ! {
    eprintln!(
        "usage: maild                  run the service\n       \
         maild ctl [json]       send one request (from the argument or stdin), print the answer\n       \
         maild add-account <gmail|yandex|imap> <email> [name] [--imap host:port --smtp host:port]\n                              \
         the password is read from stdin"
    );
    std::process::exit(2)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let r = match args.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        [] => daemon().map(|()| true),
        ["ctl"] => ctl(None),
        ["ctl", json] => ctl(Some(json)),
        ["add-account", rest @ ..] => add_account(rest),
        _ => usage(),
    };
    match r {
        Ok(ok) => std::process::exit(if ok { 0 } else { 1 }),
        Err(e) => {
            eprintln!("maild: {e}");
            std::process::exit(1)
        }
    }
}

fn daemon() -> Result<(), String> {
    let paths = config::paths();
    // First: a second maild must not start syncing next to a running one.
    let listener = ipc::listen(&paths.socket)?;
    // The schema exists before the threads open their own.
    fetch::sweep(&db::Db::open(&paths.db)?, &paths.cache);
    let hub = hub::Hub::default();
    for acc in config::load(&paths.config)? {
        hub.add_handle(account::spawn(acc, paths.clone(), hub.clone()));
    }
    smtp::spawn(paths.clone(), hub.clone());
    let h = hub.clone();
    wake::spawn(move || h.handles().iter().for_each(account::Handle::interrupt));
    eprintln!("maild: listening on {}", paths.socket.display());
    ipc::serve(listener, paths, hub);
    Ok(())
}

/// Sends one request; the answer lines follow.
fn connect(req: &Value) -> Result<std::io::Lines<BufReader<UnixStream>>, String> {
    let path = config::paths().socket;
    let mut s = UnixStream::connect(&path).map_err(|e| format!("{}: {e} (is maild running?)", path.display()))?;
    s.write_all(format!("{req}\n").as_bytes()).map_err(|e| e.to_string())?;
    Ok(BufReader::new(s).lines())
}

/// Prints the answer; true if it is `ok`.
fn once(req: &Value) -> Result<bool, String> {
    let line = connect(req)?.next().ok_or("maild closed the connection")?.map_err(|e| e.to_string())?;
    println!("{line}");
    Ok(serde_json::from_str::<Value>(&line).is_ok_and(|v| v["ok"] == true))
}

fn stdin() -> Result<String, String> {
    let mut s = String::new();
    std::io::stdin().read_to_string(&mut s).map_err(|e| format!("stdin: {e}"))?;
    Ok(s)
}

fn ctl(arg: Option<&str>) -> Result<bool, String> {
    let text = match arg {
        Some(a) => a.to_string(),
        None => stdin()?,
    };
    // Re-serialized: a pretty-printed request becomes the one line the protocol needs.
    let req: Value = serde_json::from_str(&text).map_err(|e| format!("bad json: {e}"))?;
    if req["cmd"] != "subscribe" {
        return once(&req);
    }
    for line in connect(&req)? {
        println!("{}", line.map_err(|e| e.to_string())?);
    }
    Ok(true)
}

fn server(v: Option<&&str>) -> Result<Value, String> {
    let v = v.ok_or("expected host:port")?;
    let (host, port) = v.rsplit_once(':').ok_or_else(|| format!("expected host:port, got {v}"))?;
    let port: u16 = port.parse().map_err(|_| format!("bad port: {port}"))?;
    Ok(json!({"host": host, "port": port}))
}

fn add_account(args: &[&str]) -> Result<bool, String> {
    let (mut pos, mut imap, mut smtp) = (vec![], Value::Null, Value::Null);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match *a {
            "--imap" => imap = server(it.next())?,
            "--smtp" => smtp = server(it.next())?,
            _ => pos.push(*a),
        }
    }
    let [provider, email, name @ ..] = pos.as_slice() else { usage() };
    if !matches!(*provider, "gmail" | "yandex" | "imap") {
        eprintln!("unknown provider: {provider}");
        usage();
    }
    if name.len() > 1 {
        usage();
    }
    let password = stdin()?;
    let password = password.trim_end_matches(['\n', '\r']);
    once(&json!({
        "id": 1, "cmd": "account_add", "provider": provider, "email": email, "name": name.first().unwrap_or(&""),
        "password": password, "imap": imap, "smtp": smtp,
    }))
}
