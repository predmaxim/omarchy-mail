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

The first build downloads qlitehtml from `code.qt.io` and litehtml from GitHub, so it needs network. The window is built and its tests pass before `maild` is restarted.
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

Mail is in the app menu. Its desktop entry declares the `mailto:` scheme; to make it the default handler for `mailto:` links:

```sh
xdg-mime default omarchy-mail.desktop x-scheme-handler/mailto
```

Bare web addresses (`https://…`, `www.…`) and e-mail addresses in a letter's text are links too, in plain and HTML mail: a web address opens in the browser, an e-mail address starts a new letter to it. Trailing punctuation and `&nbsp;` stay outside the link.

A `mailto:` link never goes into an unfinished new message: it gets a letter of its own, which `c` opens until it is sent or discarded. The author's dotfiles bind the window to `Super+Shift+M`. There is one window per system: a second launch hands its arguments to the open window and exits.

The folder picker has Flagged across all mailboxes and in each mailbox, right after its Inbox.

A flagged message has a red bar at its left edge (the theme's red, `urgent`), an unread one an accent bar; both together sit side by side, red at the edge. Red is the user's choice: Omarchy rules §9 keep own row marks accent.

One cursor moves through the whole window; the letters work in the Russian layout too (they are matched by key code).

| Key | Action |
|---|---|
| Up / Down | move between messages; the message opens at once |
| Right / Left | into the message / back to the list; inside a row, between buttons, attachments, links |
| Enter | on a message in the list: into the message, as Right; press the button; open the attachment or link |
| PgUp / PgDn | scroll the message by a page, from the list too |
| Home / End | in the list: the first / last loaded message, it opens at once; in the message text: to its top / its end |
| `d`, Del | delete (to Trash), cursor to the next one; in Trash and Spam it deletes forever, on the second press only |
| `r` | reply |
| `c` | compose |
| `u` | read / unread |
| Shift+`u` | unread filter |
| `f` | flag / unflag; in Flagged an unflagged message stays until the cursor leaves it; the Flag button on top does the same |
| `o` | open the original in the browser |
| `y` | copy the message text |
| Ctrl+C | copy the text selected in the message or its header |
| `s` | save the attachment to Downloads |
| Space | preview the attachment in `omarchy-quicklook`; scroll the message |
| `/` | search; typing goes into the field |
| `a` / `g` | open the mailbox / folder list (not in the search field or the form); `a` only with two mailboxes or more |
| Ctrl+Z | undo the last action |
| Ctrl + / Ctrl − / Ctrl 0 | letter zoom: larger / smaller / back to GNOME's text size (Ctrl+wheel too); the list keeps Omarchy's size; remembered in `~/.config/omarchy-mail/window.json` |
| Esc | one step back: from the message to the list; in the list clear the selection, leave search, then up to the mailbox and folder pickers; in the search field clear it, then up to the pickers; leave the form or settings. Esc never closes the window: close it with Hyprland. |
| Ctrl+Enter, Ctrl+V, Ctrl+O | in the form: send, paste a screenshot, attach a file |
| Ctrl+D | in the form: discard the draft; press twice |

Selecting several messages works as in a file manager:

| Key | Action |
|---|---|
| Ctrl+Up / Down | move the cursor, selection stays |
| Ctrl+Space | mark / unmark the message under the cursor |
| Shift+Up / Down | range from the anchor to the cursor |
| Shift+Home / End, Ctrl+Home / End | as Shift / Ctrl with Up / Down, to the first / last loaded message |
| plain Up / Down | clear the selection |
| Ctrl+A | all shown messages: the loaded rows of the folder or of the search results |
| Ctrl+click, Shift+click | one message, a range |
| Esc | clear the selection |

`d`, `u`, `f` act on all selected messages and Ctrl+Z undoes the whole batch; without a selection they act, as `r`, `o`, `y` do, on the open message.

The mailboxes page (the gear) has keys of its own:

| Key | Action |
|---|---|
| Up / Down | the "Letter text in the list" switch, the mailboxes and "Add a mailbox"; the form below shows the row under the cursor |
| Enter | on the switch: flip it; on a row: go into the form; in the form's last field: Save (Add) |
| Space, Left / Right | flip the switch (a click too): show or hide the letter text under the subjects in the list |
| Up / Down, Tab, Shift+Tab | between the form's fields; Tab on a row goes into its form |
| `d`, Del | remove the mailbox, on the second press only |
| Esc | from the form back to its row, from the list back to the mail |

The form of a mailbox shows its address and type, and edits the sender name and a new app password (empty: unchanged); Other IMAP also its login and servers. Save tries a new password, login or IMAP server with a trial login first and stores nothing if it fails; the mailbox's mail, queued actions and outbox stay. A refused login shows here with "Retry login". Edits not saved are dropped when the cursor moves to another row.

Files of the window:

| What | Path |
|---|---|
| Drafts | `$XDG_DATA_HOME/omarchy-mail/drafts/` (`~/.local/share/omarchy-mail/drafts/`) |
| Window cache (images, originals for the browser) | `$XDG_CACHE_HOME/omarchy-mail/window/` (`~/.cache/omarchy-mail/window/`) |
| Window settings | `$XDG_CONFIG_HOME/omarchy-mail/window.json` (`~/.config/omarchy-mail/window.json`): `letterZoom` (letter zoom), `listPreview` (`false`: no letter text in the list) |

The window talks to the network only to load the images of the open message (`http`/`https`, no cookies, cached for 30 days). Everything else goes through `maild`.

### Licenses

The window renders HTML mail with [qlitehtml](https://code.qt.io/cgit/playground/qlitehtml.git) (GPL-3.0 WITH Qt-GPL-exception-1.0) and [litehtml](https://github.com/litehtml/litehtml) (BSD). qlitehtml is built from `code.qt.io` at 180e3ebb and litehtml from github.com/litehtml/litehtml at master a5ee957801d4, each with its patch, `app/third_party/qlitehtml.patch` and `app/third_party/litehtml.patch`. `qlitehtml.patch` also ports qlitehtml to litehtml master's API (its own submodule still pins an older litehtml). Besides the window's own changes, `litehtml.patch` fixes table bugs still present upstream: column percentages that add up to more than 100 % (later columns get only what is left, as in css-tables-3), and a table cell's `height`, which litehtml ignored (border-box in quirks mode, as in browsers). It also fixes layout regressions of litehtml master: a picture with a percentage `max-width` shrinks to its cell; a table's (or a `box-sizing: border-box` block's) `width`, `min-width` and `max-width` hold its padding and borders when a cell is sized around it; a picture with only a height takes the width its aspect ratio gives; a cell's `min-width` and `max-width` bound its column, not the width the table gives it.

## Where things live

| What | Path |
|---|---|
| Accounts | `$XDG_CONFIG_HOME/omarchy-mail/accounts.toml` (`~/.config/omarchy-mail/`) |
| Database | `$XDG_DATA_HOME/omarchy-mail/mail.db` (`~/.local/share/omarchy-mail/`) |
| Cache | `$XDG_CACHE_HOME/omarchy-mail` (`~/.cache/omarchy-mail`) |
| Socket | `$XDG_RUNTIME_DIR/omarchy-mail.sock` |
| Passwords | keyring, `service=omarchy-mail account=<email>` |
