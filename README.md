# omarchy-mail

A mail client for Omarchy. `maild` is its user service: it syncs IMAP mailboxes
(IDLE) into SQLite, shows notifications and serves a local socket API. The Qt 6 / QML
window in `app/` is its client.

License: GPL-3.0.

## Requirements

- Rust (`cargo`) to build; OpenSSL (used by `native-tls`)
- For the window: `cmake`, `ninja`, `git`, `qt6-base`, `qt6-declarative` (`install.sh` installs missing ones)
- `libsecret` (`secret-tool`): passwords are kept in the Secret Service keyring
- A notification daemon (Omarchy ships one) and a systemd user session

## Install

```sh
./install.sh     # builds and tests the window, installs it, maild and the user unit, starts the service
./uninstall.sh   # removes the binaries, unit and desktop entry; mail data and passwords are kept
```

The first build downloads qlitehtml and litehtml from `code.qt.io`, so it needs network. The window is built and its tests pass before `maild` is restarted.
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
- Actions (read/unread, flag/unflag, delete, delete forever, send) made offline are queued and replayed when the connection returns; sent mail goes through an SMTP outbox.
- Up to 3 new Inbox messages get a notification each; more arrive as one "N new messages" notification. Mail already there at the first sync is not announced.

## Window

```sh
omarchy-mail                  # open the window (a second launch raises the open one)
omarchy-mail --open <id>      # open message <id>
omarchy-mail mailto:a@b.org?subject=Hi   # new message form, fields filled in
```

Mail is in the app menu, registered as the `mailto:` handler; Omarchy binds it to `Super+Shift+M` in its dotfiles. There is one window per system: a second launch hands its arguments to the open window and exits.

One cursor moves through the whole window; the letters work in the Russian layout too (they are matched by key code).

| Key | Action |
|---|---|
| Up / Down | move between messages; the message opens at once |
| Right / Left | into the message / back to the list; inside a row, between buttons, attachments, links |
| Enter | press the button; open the attachment or link |
| `d`, Del | delete (to Trash), cursor to the next one; in Trash and Spam it deletes forever, on the second press only |
| `r` | reply |
| `c` | compose |
| `u` | read / unread |
| Shift+`u` | unread filter |
| `f` | flag / unflag |
| `o` | open the original in the browser |
| `y` | copy the message text |
| `s` | save the attachment to Downloads |
| Space | preview the attachment in `omarchy-quicklook`; scroll the message |
| `/` | search; typing goes into the field |
| Ctrl+Z | undo the last action |
| Esc | one step back: clear the selection, leave search / form / settings, then close the window |
| Ctrl+Enter, Ctrl+V, Ctrl+O | in the form: send, paste a screenshot, attach a file |
| Ctrl+D | in the form: discard the draft; press twice |

Selecting several messages works as in a file manager:

| Key | Action |
|---|---|
| Ctrl+Up / Down | move the cursor, selection stays |
| Ctrl+Space | mark / unmark the message under the cursor |
| Shift+Up / Down | range from the anchor to the cursor |
| plain Up / Down | clear the selection |
| Ctrl+A | all messages of the folder or search results |
| Ctrl+click, Shift+click | one message, a range |

`d`, `u`, `f` act on all selected messages and Ctrl+Z undoes the whole batch; `r`, `o`, `y` act on the message under the cursor.

Files of the window:

| What | Path |
|---|---|
| Drafts | `$XDG_DATA_HOME/omarchy-mail/drafts/` (`~/.local/share/omarchy-mail/drafts/`) |
| Window cache (images, originals for the browser) | `$XDG_CACHE_HOME/omarchy-mail/window/` (`~/.cache/omarchy-mail/window/`) |

The window talks to the network only to load the images of the open message (`http`/`https`, no cookies, cached for 30 days). Everything else goes through `maild`.

### Licenses

The window renders HTML mail with [qlitehtml](https://code.qt.io/cgit/playground/qlitehtml.git) (GPL-3.0 WITH Qt-GPL-exception-1.0) and [litehtml](https://github.com/litehtml/litehtml) (BSD). Both are built from `code.qt.io` at pinned commits with two patches, `app/third_party/qlitehtml.patch` and `app/third_party/litehtml.patch`.

## Where things live

| What | Path |
|---|---|
| Accounts | `$XDG_CONFIG_HOME/omarchy-mail/accounts.toml` (`~/.config/omarchy-mail/`) |
| Database | `$XDG_DATA_HOME/omarchy-mail/mail.db` (`~/.local/share/omarchy-mail/`) |
| Cache | `$XDG_CACHE_HOME/omarchy-mail` (`~/.cache/omarchy-mail`) |
| Socket | `$XDG_RUNTIME_DIR/omarchy-mail.sock` |
| Passwords | keyring, `service=omarchy-mail account=<email>` |
