//! Live check: `cargo run --example sync_once -- <email>` syncs one account from accounts.toml
//! into /tmp/maild-test.db and prints folders and message counts per role. Read-only on the server.
#![allow(dead_code)]

#[path = "../src/config.rs"]
mod config;
#[path = "../src/db.rs"]
mod db;
#[path = "../src/imap.rs"]
mod imap;
#[path = "../src/mime.rs"]
mod mime;
#[path = "../src/secret.rs"]
mod secret;
#[path = "../src/sync.rs"]
mod sync;

use std::collections::BTreeMap;

fn run(email: &str) -> Result<(), String> {
    let acc = config::load(&config::paths().config)?.into_iter().find(|a| a.email == email).ok_or(format!("{email}: not in accounts.toml"))?;
    let db = db::Db::open(std::path::Path::new("/tmp/maild-test.db"))?;
    let mut conn = imap::Conn::connect(&acc, &secret::lookup(email)?)?;
    let mut new = vec![];
    sync::sync_account(&mut conn, &db, &acc, |ids| new.extend(ids))?;
    let folders = db.folders_of(email)?;
    let mut by_role: BTreeMap<&str, usize> = BTreeMap::new();
    for f in &folders {
        let n = db.list(&db::ListQuery { folder: Some(f.id), ..Default::default() })?.len();
        println!("{:7} {:6} {}", f.role.as_str(), n, f.path);
        *by_role.entry(f.role.as_str()).or_default() += n;
    }
    println!("folders: {}, new this run: {}, by role: {by_role:?}", folders.len(), new.len());
    Ok(())
}

fn main() {
    let Some(email) = std::env::args().nth(1) else {
        eprintln!("usage: sync_once <email>");
        std::process::exit(2)
    };
    if let Err(e) = run(&email) {
        eprintln!("{e}");
        std::process::exit(1)
    }
}
