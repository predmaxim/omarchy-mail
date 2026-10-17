#![allow(dead_code)] // consumed by later tasks

mod config;
mod db;
mod i18n;
mod imap;
mod mime;
mod secret;
mod sync;

fn usage() -> ! {
    eprintln!("usage: maild [ctl [json] | add-account <provider> <email> [name]]");
    std::process::exit(2)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        [] => {}
        ["ctl"] | ["ctl", _] => {}
        ["add-account", p, _email] | ["add-account", p, _email, _] => {
            if !matches!(*p, "gmail" | "yandex" | "imap") {
                eprintln!("unknown provider: {p}");
                usage();
            }
        }
        _ => usage(),
    }
    eprintln!("not implemented yet");
    std::process::exit(1)
}
