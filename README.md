# omarchy-mail

A mail client for Omarchy. `maild` is its user service: it syncs IMAP mailboxes
(IDLE) into SQLite, shows notifications and serves a local socket API.

License: GPL-3.0.

## Requirements

- Rust (`cargo`) to build; OpenSSL (used by `native-tls`)
- `libsecret` (`secret-tool`): passwords are kept in the Secret Service keyring
- A notification daemon (Omarchy ships one) and a systemd user session

## Install

```sh
./install.sh     # builds, installs ~/.local/bin/maild and the user unit, starts it
./uninstall.sh   # removes the binary and unit; mail data and passwords are kept
```

Run `./install.sh` again to update. Logs: `journalctl --user -u maild -f`.

## Adding accounts

Gmail and Yandex need 2-step verification and an app password, not the account password:
[Google app passwords](https://myaccount.google.com/apppasswords); Yandex ID, Security, App passwords.

```sh
read -rs P; printf %s "$P" | maild add-account gmail you@gmail.com "Your Name"
read -rs P; printf %s "$P" | maild add-account yandex you@yandex.ru "Your Name"
read -rs P; printf %s "$P" | maild add-account imap you@example.org "Your Name" \
  --imap mail.example.org:993 --smtp mail.example.org:465
```

The password is read from stdin, checked with a trial IMAP login and stored in the keyring.

## Talking to the service

```sh
maild ctl '{"cmd":"status"}'
maild ctl '{"cmd":"folders","account":"you@gmail.com"}'
maild ctl '{"cmd":"search","q":"invoice"}'
maild ctl '{"cmd":"subscribe","window":true}'   # stream events
```

The full socket protocol is in [maild/PROTOCOL.md](maild/PROTOCOL.md).

## Behaviour

- Syncs mail since 2026-01-01; older mail is found by server-side search.
- IMAP IDLE keeps each mailbox current; bodies and attachments are fetched on demand.
- Actions (read, move, delete, send) made offline are queued and replayed when the connection returns; sent mail goes through an SMTP outbox.
- A notification is shown for each new message (not for mail that was already there at start).

## Where things live

| What | Path |
|---|---|
| Accounts | `$XDG_CONFIG_HOME/omarchy-mail/accounts.toml` (`~/.config/omarchy-mail/`) |
| Database | `$XDG_DATA_HOME/omarchy-mail/mail.db` (`~/.local/share/omarchy-mail/`) |
| Cache | `$XDG_CACHE_HOME/omarchy-mail` (`~/.cache/omarchy-mail`) |
| Socket | `$XDG_RUNTIME_DIR/omarchy-mail.sock` |
| Passwords | keyring, `service=omarchy-mail account=<email>` |
