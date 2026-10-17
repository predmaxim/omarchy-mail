# maild socket protocol

This is the contract between `maild` (the service) and its clients: the `omarchy-mail`
window and `maild ctl`. Only this file defines it.

## Transport

- Unix stream socket `$XDG_RUNTIME_DIR/omarchy-mail.sock`. Without `XDG_RUNTIME_DIR` the path
  is `/run/user/<uid>/omarchy-mail.sock`. The socket has mode `0600`.
- At start `maild` removes a stale socket file. If another `maild` already answers on the
  socket, the new one refuses to start.
- Framing: one JSON object per line, UTF-8, ended by `\n`. Clients must not put raw newlines
  inside a message; compact JSON never has them. Blank lines are ignored.
- A request line longer than 1 MiB (1 048 576 bytes) gets an error and the connection is closed.
- One connection carries any number of requests. They are answered one at a time, in the order
  sent. A slow request delays the requests behind it on the same connection; `account_add`
  waits for a trial login (up to about 70 s). Run slow requests on a second connection if that
  matters.
- After `subscribe`, events arrive on the same connection, between responses. A response has
  `id` and `ok`; an event has `event`.
- A client that does not read, so that a write to it blocks for 10 s, is disconnected. It can
  reconnect and subscribe again.
- Key order inside objects is not significant. Ignore unknown fields: later versions may add
  fields.

## Requests and responses

```json
{"id": 1, "cmd": "folders", "account": "me@gmail.com"}
```

- `cmd` selects the command. The other fields are its parameters; unknown fields are ignored.
- `id` is any JSON value. The response echoes it (`null` if the request had none).

Success:

```json
{"id": 1, "ok": true, "result": <value>}
```

A command with nothing to return leaves out `result`: `{"id": 1, "ok": true}`.

Failure:

```json
{"id": 1, "ok": false, "error": "текст ошибки"}
```

- `error` is a ready-to-show string. The service's own messages ("no such account", "the
  mailbox is already added", …) follow the service's language. That language is the first
  non-empty of `LC_ALL`, `LC_MESSAGES`, `LANG` in the service's environment: `ru*` means
  Russian, anything else English.
- Errors from servers, the keyring or the file system are passed on as they are.
- Malformed requests get English parser text. Examples: bad JSON, an unknown `cmd`, a missing
  or mistyped field. Such errors mean a client bug.

## Commands

| `cmd` | Parameters | `result` |
|---|---|---|
| `subscribe` | `window: bool` (default `false`) | — |
| `status` | — | `{accounts: [{email, status, queued}]}` |
| `accounts` | — | `Account[]` |
| `account_add` | `provider, email, name?, password, login?, imap?, smtp?` | `Account` |
| `account_remove` | `email` | — |
| `folders` | `account?` | `FolderInfo[]` |
| `list` | `account?, role?, folder?, unread_only?, offset?, limit?` | `Summary[]` |
| `message` | `message` (message id; negative: an outbox message) | `Full` |
| `search` | `q` | `{local: Summary[], token}` |
| `contacts` | `prefix` | `Addr[]` |
| `sync` | `account?, folder?` | — |
| `attachment` | `message, index` | `{path}` |
| `open_remote` | `account, folder_path, uid` | `{id}` |
| `act` | `ids, op` | `{batch}` |
| `undo` | `batch` | `{batch}` |
| `send` | a `Draft` | `{outbox}` |
| `outbox_retry` | `outbox` | — |
| `outbox_discard` | `outbox` | — |

`?` marks an optional parameter: leave it out or send `null`.

The message id parameter is named `message`, not `id`. `id` is the request id.

### `subscribe`

```json
→ {"id": 1, "cmd": "subscribe", "window": true}
← {"id": 1, "ok": true}
```

- From now on every event is sent to this connection until it closes.
- Subscribing twice on one connection is an error.
- `window: true` marks the subscriber as a window. A click on a new-mail notification then sends
  `open {id}` to the subscribers instead of starting `omarchy-mail --open <id>`. Only the
  window sets it; `maild ctl` subscribes with `false`.
- Events are sent as they happen, in order, also while a request on this connection is still
  being answered.
- Subscribing gives no initial state. Call `status`, `folders` and `list` afterwards.

### `status`

```json
→ {"id": 2, "cmd": "status"}
← {"id": 2, "ok": true, "result": {"accounts": [
     {"email": "me@gmail.com", "status": "online", "queued": 0},
     {"email": "me@yandex.ru", "status": {"offline": "imap.yandex.ru: Network is unreachable"}, "queued": 3}]}}
```

- One entry for each running account. The order has no meaning: use `accounts` for the order
  of the configuration.
- `status` is a `Status`.
- `queued` is the number of actions still to reach the server plus the messages waiting in the
  outbox. This gives the line "нет сети · 3 в очереди".

### `accounts`

```json
→ {"id": 3, "cmd": "accounts"}
← {"id": 3, "ok": true, "result": [{"email": "me@gmail.com", "name": "Max", "provider": "gmail",
     "login": "me@gmail.com", "imap": {"host": "imap.gmail.com", "port": 993},
     "smtp": {"host": "smtp.gmail.com", "port": 465}, "append_sent": false}]}
```

The accounts as configured (`accounts.toml`), in order. Passwords are never returned: they
live in the keyring only.

### `account_add`

```json
→ {"id": 4, "cmd": "account_add", "provider": "gmail", "email": "me@gmail.com", "name": "Max",
   "password": "<app password>"}
→ {"id": 5, "cmd": "account_add", "provider": "imap", "email": "me@example.org", "name": "Max",
   "password": "…", "login": "me", "imap": {"host": "mail.example.org", "port": 993},
   "smtp": {"host": "mail.example.org", "port": 465}}
← {"id": 4, "ok": true, "result": <Account>}
```

- `provider` is `gmail`, `yandex` or `imap`.
- `gmail` and `yandex` use built-in servers (IMAP 993 and SMTP 465, TLS); the service ignores
  `imap`, `smtp` and `login` for them. Yandex 360 mailboxes on their own domain use `yandex`.
- `imap` requires `imap` and `smtp` as `{host, port}`; both use TLS. `login` defaults to the
  email. Mail sent through such an account is also appended to its Sent folder.
- `name` is the sender name for "From"; it may be empty.
- Order of work:
  1. Validate the fields.
  2. Refuse a duplicate email (case-insensitive).
  3. Trial IMAP login with the password. On failure the server's error comes back, and nothing
     is stored.
  4. Store the password in the keyring.
  5. Append the account to `accounts.toml`.
  6. Start its sync.
- The password is never written to a log or into any response.
- After the call, `status` events follow (`connecting` → `online`), then `changed` events as
  the first sync brings in mail.

### `account_remove`

```json
→ {"id": 6, "cmd": "account_remove", "email": "me@gmail.com"}
← {"id": 6, "ok": true}
```

- Order of work:
  1. Stop the account's threads and wait for them.
  2. Delete its folders, messages, queued actions and outbox from the database.
  3. Delete its password from the keyring.
  4. Remove it from `accounts.toml`.
- Nothing is deleted on the server.
- Events follow: `status` without the account, and `changed {folders: true, ids: []}`.

### `folders`

```json
→ {"id": 7, "cmd": "folders"}
← {"id": 7, "ok": true, "result": [
     {"account": null, "id": null, "path": "inbox", "name": "inbox", "role": "inbox", "unread": 12},
     {"account": null, "id": null, "path": "flagged", "name": "flagged", "role": "flagged", "unread": 0},
     {"account": null, "id": null, "path": "sent", "name": "sent", "role": "sent", "unread": 0},
     {"account": null, "id": null, "path": "trash", "name": "trash", "role": "trash", "unread": 1},
     {"account": null, "id": null, "path": "junk", "name": "junk", "role": "junk", "unread": 4}]}
→ {"id": 8, "cmd": "folders", "account": "me@yandex.ru"}
← {"id": 8, "ok": true, "result": [
     {"account": "me@yandex.ru", "id": 3, "path": "INBOX", "name": "INBOX", "role": "inbox", "unread": 5},
     {"account": "me@yandex.ru", "id": 4, "path": "&BB4EQgQ,BEAEMAQyBDsENQQ9BD0ESwQ1-", "name": "Отправленные", "role": "sent", "unread": 0}]}
```

- Without `account` the result is the unified view, all mailboxes together. It always has these
  five entries in this order: Inbox, Flagged, Sent, Trash, Junk. `id` and `account` are `null`;
  `path` and `name` are the role name, and the window translates it. Open such an entry with
  `list {role}`.
- With `account` the result is that mailbox's folders from the server, in discovery order.
  Open one with `list {folder: id}`. The `remote` folders (opened server search hits) are not
  listed.
  - `name` is for display: the decoded last part of the path.
  - `path` is the raw IMAP name. Never show it, and never decode it yourself.
- `unread` counts unread messages. For `flagged` it counts unread flagged messages outside
  Trash and Junk.

### `list`

```json
→ {"id": 9, "cmd": "list", "role": "inbox", "unread_only": false, "offset": 0, "limit": 50}
→ {"id": 10, "cmd": "list", "folder": 3, "limit": 50}
← {"id": 9, "ok": true, "result": [<Summary>, …]}
```

- `account` limits the list to one mailbox.
- `role` takes precedence over `folder`.
  - `role: "flagged"` lists flagged messages of every folder except Trash and Junk.
  - Any other role lists the folders with that role.
- `folder` is a folder id from `folders {account}`.
- With neither `role` nor `folder`, the list holds every message except opened server search
  hits (role `remote`; `role: "remote"` lists those).
- `unread_only` (default `false`) lists unseen messages only.
- `offset` and `limit` page the list. A `limit` of `0` or a missing one means no limit; always
  send one.
- Order: newest first (`date`, then `id`).
- `role: "sent"` without `unread_only` puts the outbox first: messages waiting to be sent,
  newest first, with `queued: true` and a negative `id`. Paging counts them like other rows.

### `message`

```json
→ {"id": 11, "cmd": "message", "message": 42}
← {"id": 11, "ok": true, "result": <Full>}
```

- An unknown id is an error ("message not found").
- `has_body: false` means a big message (over 2 MiB) of which sync took no text part (it has
  none, or the server sent no structure): `text` is empty and `html` is `null`. A big message
  whose text parts sync took has `has_body: true`.
- A message with `has_body: false` is fetched first: the reply waits up to 30 s, then errors
  ("no network"). The `attachments` of a fetched big message are the list `attachment` numbers by.
- A message that has a body is answered at once, never waiting for the network. If its inline
  images are not cached, the service fetches them in the background (at most every 5 minutes per
  message) and sends `changed` with the message id when it caches something: re-request then.
- After a full fetch of a big message its `attachments` list (and so the indices) may change:
  re-read the message (on `changed` with its id) before using an index.
- The result gains `inline: {"<cid>": "/absolute/path"}`: the cached files of the images that
  `html` references as `cid:` (`{}` for none). If only the images could not be fetched, the
  message is still returned, without them in `inline`.
- Error while a local move waits for the server (`uid` is `null`): "message is being moved".
- A negative id (an outbox row of `list {role: "sent"}`) returns that outbox message, parsed from
  its text, in the same shape, with `summary.queued` / `summary.failed` and `send_error: string`
  for a failed one; `inline` is `{}`. Attachments of outbox messages are not served.

### `attachment`

```json
→ {"id": 13, "cmd": "attachment", "message": 42, "index": 0}
← {"id": 13, "ok": true, "result": {"path": "/absolute/file"}}
```

- The service downloads attachment `index` (`Attachment.index`) into its cache,
  `<cache>/<message id>/<index>/<name>` (the name is cleaned of `/`, `\` and control characters; an empty
  one becomes `attachment-<index>`), and returns the absolute path. A file that is already there is
  returned at once, with no network.
- Errors: no network or no answer within 30 s, the message is being moved (`uid` is `null`).

### `search`

```json
→ {"id": 12, "cmd": "search", "q": "договор постав"}
← {"id": 12, "ok": true, "result": {"local": [<Summary>, …], "token": 7}}
```

- `local` holds up to 200 local messages, newest first.
  - Every word of `q` must match the start of a word in the subject, sender, recipients or text.
    Words with no letter or digit (`№`, `-`) are ignored.
  - Matching ignores case, and works for Cyrillic.
  - An empty `q` gives `[]`.
- `token` grows with every `search`, on any connection.
- Each mailbox also searches its server for mail before 2026-01-01: every word of `q` must be in
  the subject, sender or text (as a substring).
  Gmail: the "All Mail" folder; others: every folder but Junk. Up to 30 newest hits per mailbox.
  Each mailbox sends its hits as a `search_remote {token, hits}` event, also with no hits. Drop
  events whose `token` is not the latest one you got.
- The query is cut to 256 characters, control characters count as spaces; a blank query searches no server.
- If a mailbox's search failed, its event has `hits: []` and `error: string`. If only some of its
  folders failed, `hits` holds the rest and `error` the first failure.
- Hits are `Summary` with `id: 0`, `role: "remote"` and `remote: {account, folder_path, uid}`;
  pass `remote` to `open_remote`.

### `contacts`

```json
→ {"id": 13, "cmd": "contacts", "prefix": "ив"}
← {"id": 13, "ok": true, "result": [{"name": "Иван Петров", "addr": "ivan@example.org"}]}
```

- Returns up to 10 addresses for "To" completion.
- The addresses are the senders and recipients of mail in Sent folders, most frequent first.
- `prefix` matches anywhere in the address or the name, case-insensitive.

### `sync`

```json
→ {"id": 14, "cmd": "sync"}
→ {"id": 15, "cmd": "sync", "account": "me@gmail.com"}
→ {"id": 16, "cmd": "sync", "folder": 4}
← {"id": 14, "ok": true}
```

- Without `folder` this is "sync now": a full sync of the account, or of every account when
  `account` is missing. It first sends the queued actions (see `act`). It also retries an
  account in `auth_failed` or waiting to reconnect.
- With `folder` (a folder id) it syncs that folder only. `account` is optional here; the folder
  knows its account. Use it when the user opens a folder.
- The call returns at once. The results come as `changed` events: a full sync sends one for each
  folder that got new mail as soon as that folder is done, then one with `ids: []`.
- Errors: an unknown `account` or `folder`.

### `act`

```json
→ {"id": 17, "cmd": "act", "ids": [101, 102], "op": "delete"}
← {"id": 17, "ok": true, "result": {"batch": 5}}
```

- Messages opened with `open_remote` (role `remote`) refuse every `op`; the call fails and changes nothing.
- `op` is one of:
  - `read`, `unread`, `flag`, `unflag`: set or clear "seen" or "flagged".
  - `delete`: move to the Trash of the message's mailbox. Messages already in Trash are left
    alone. If a mailbox has no Trash folder, this is an error and nothing changes.
  - `delete_forever`: allowed only for messages in Trash or Junk. Any other message in `ids` is an
    error, and nothing changes.
- `ids` may mix mailboxes; one batch covers them all. Ids that no longer exist are skipped. Ids
  of `0` or below (outbox rows, server search hits) are an error.
- The local database changes at once, before the answer; a `changed {folders: true, ids}` event
  follows. The servers follow from a queue, in order. The queue survives a restart and waits for
  the network; `status` counts it in `queued`.
- Messages already in the wanted state are left out of the batch, so `undo` reverts only what this
  call changed. `batch: 0` means nothing changed; there is nothing to undo then.
- A moved message keeps its `id`; its `uid` is `null` until the server has moved it. If the
  server's copy cannot be found in the target folder afterwards, the message leaves the list and
  comes back with that folder's next sync, under a new `id`.
- A message that is no longer on the server (deleted from a phone) is skipped silently; the next
  sync of its folder removes it from the list. A delete the server could not do this way (its
  Trash is gone) puts the message back in its folder.
- When a server refuses an action for another reason, the message comes back as it was and
  `action_failed {batch, error}` is sent, once for the batch. An action that fails on the
  connection 5 times counts as refused, so it cannot hold up the queue. A message deleted forever
  comes back by downloading it again.
- `restore` is not accepted here: it is what `undo` of a delete does.

### `undo`

```json
→ {"id": 18, "cmd": "undo", "batch": 5}
← {"id": 18, "ok": true, "result": {"batch": 6}}
```

- Reverts a batch with a new one, queued like any other: a deleted message goes back to the folder
  it came from, read ↔ unread, flag ↔ unflag. The new batch can be undone too: an undone delete is
  deleted again.
- Only what is still there is reverted: messages gone since, and actions the server refused (they
  are reverted already), are skipped. The result may be `batch: 0`.
- Errors: an unknown batch or one with nothing left to undo, and a `delete_forever` batch: deleting
  forever cannot be undone.

### `open_remote`

```json
→ {"id": 19, "cmd": "open_remote", "account": "me@gmail.com", "folder_path": "[Gmail]/All Mail", "uid": 7}
← {"id": 19, "ok": true, "result": {"id": 230}}
```

- Downloads a server search hit (`Summary.remote`) into the account's `remote` folders.
- Returns the local id for `message`; opening the same hit again returns the same id.
- Waits for the server up to 30 s.

### `send`

```json
→ {"id": 20, "cmd": "send", "account": "me@gmail.com", "to": [{"name": "Иван", "addr": "ivan@example.org"}],
   "subject": "Re: счёт", "text": "…", "reply_to": 42, "attachments": ["/home/me/счёт.pdf"]}
← {"id": 20, "ok": true, "result": {"outbox": 3}}
```

- The request is a `Draft`: `account, to: Addr[], subject, text, reply_to?: i64,
  attachments?: [path]`.
- `to` must not be empty. `subject` is used as given (the window adds `Re:`). `text` is plain
  text. `reply_to` is a message id: the mail gets `In-Reply-To` and `References` from it.
- `attachments` are absolute paths on the machine of `maild`; a file that cannot be read makes
  the request fail and nothing is queued.
- The message is queued: it shows in `list {role: "sent"}` as `queued` (negative `id` =
  `-outbox`), and is sent when online; a network error retries every 30 s. Mail on servers that
  do not file sent mail themselves (`append_sent`) is copied to the Sent folder afterwards.
- With `append_sent`, after the server took the mail the row stays in Sent (`queued: false`)
  until `maild` has copied it to the Sent folder; a failed copy is retried at the next sync.
  The mail is never sent twice.
- Without `append_sent` (the server files sent mail itself, as Gmail does) the row leaves the
  outbox once the server took the mail, and the account's Sent folder is synced right away.
- A queued message discarded before its turn is not sent.
- When the server refuses it permanently, `send_failed {outbox, error}` is sent and the row
  stays in Sent with `failed: true` (and still `queued: true`: it is an outbox row).

### `outbox_retry` and `outbox_discard`

```json
→ {"id": 21, "cmd": "outbox_retry", "outbox": 3}
→ {"id": 22, "cmd": "outbox_discard", "outbox": -3}
← {"id": 21, "ok": true}
```

- `outbox` is the id `send` returned; the negative id `list` shows works too.
- `outbox_retry` queues a failed message again. Error if the message is not failed.
- `outbox_discard` deletes a queued or failed message from the outbox. Error for one the server
  has taken already (`queued: false`) or one that is gone.
- Both are followed by a `changed` event.

## Events

They go to subscribed connections only, one JSON object per line.

| `event` | Fields | When |
|---|---|---|
| `changed` | `folders: bool, ids: [i64]` | Mail changed. `ids` are added or re-fetched messages; the list may be empty (flags changed, messages deleted, an account removed). `folders: true` means unread counts may have changed: reload `folders`. Reload the visible list in both cases. |
| `status` | `accounts: [{email, status}]` | An account's `Status` changed, or an account was removed. Carries every account; for `queued`, call `status`. |
| `open` | `id: i64` | A new-mail notification was clicked while a window is subscribed: show message `id` and raise the window. |
| `search_remote` | `token: u64, hits: Summary[], error?: string` | Server search hits of one mailbox. `error` only when its search failed. |
| `action_failed` | `batch: i64, error: string` | A server refused an action of `batch` (see `act`). The database is already back to the previous state. |
| `send_failed` | `outbox: i64, error: string` | Sending failed permanently. The message stays in the outbox as not sent (`Summary.failed`). |

```json
{"event": "changed", "folders": true, "ids": [101, 102]}
{"event": "status", "accounts": [{"email": "me@gmail.com", "status": "online"}, {"email": "me@yandex.ru", "status": "auth_failed"}]}
{"event": "open", "id": 101}
{"event": "search_remote", "token": 7, "hits": [<Summary>]}
{"event": "search_remote", "token": 7, "hits": [], "error": "…"}
{"event": "action_failed", "batch": 5, "error": "…"}
{"event": "send_failed", "outbox": 2, "error": "…"}
```

## Types

`Status` — the state of an account's connection:

| Value | Meaning |
|---|---|
| `"connecting"` | Connecting or reconnecting. |
| `"online"` | Connected; new mail is noticed at once. |
| `{"offline": "reason"}` | No connection; reconnects on its own. The reason is passed on as the service got it. |
| `"auth_failed"` | The server rejected the login. No retries until `sync` or a new password. |

`Role`: `inbox`, `sent`, `trash`, `junk`, `flagged`, `other`, `remote`.
- `remote` holds server search hits opened with `open_remote`.
- `flagged` only names the unified "Flagged" entry and filter.

`Addr`: `{name: string, addr: string}`. `name` may be empty.

`Account`:

```json
{"email": "…", "name": "…", "provider": "gmail|yandex|imap", "login": "…",
 "imap": {"host": "…", "port": 993}, "smtp": {"host": "…", "port": 465}, "append_sent": false}
```

`FolderInfo`:

```json
{"account": "…" | null, "id": 3 | null, "path": "…", "name": "…", "role": "<Role>", "unread": 5}
```

`Summary` — one row of a list:

```json
{"id": 42, "account": "me@gmail.com", "role": "inbox", "folder_path": "INBOX", "date": 1767312000,
 "from": {"name": "Иван", "addr": "ivan@example.org"}, "subject": "…", "preview": "…",
 "seen": false, "flagged": false, "has_attachments": true, "queued": false, "failed": false}
```

- `date` is in Unix seconds; `0` means the message has no valid date.
- `preview` is the first part of the text as one plain line.
- `has_attachments` is true when there is an attachment that is not inline.
- `queued: true` marks an outbox message: `id` is negative, `folder_path` is empty and `from` is
  the account. `failed: true` (outbox messages only; otherwise `false`) means the server
  refused the message for good: show it as not sent.
- Server search hits have `id: 0`, `role: "remote"` and an extra
  `remote: {account, folder_path, uid}` for `open_remote`. The field is missing on local
  messages.

`Attachment`:

```json
{"index": 0, "name": "счёт.pdf", "mime": "application/pdf", "size": 48213, "cid": null, "inline": false}
```

- `index` is the number to pass to `attachment`.
- `cid` is the Content-ID, without angle brackets.
- `inline: true` marks an image used by the HTML body.

`Full` — one message:

```json
{"summary": <Summary>, "to": [<Addr>], "text": "…", "html": "…" | null,
 "attachments": [<Attachment>], "has_body": true, "message_id": "…", "in_reply_to": "…",
 "refs": "…", "folder": 3, "uid": 1234 | null}
```

- `html` is already sanitized: no scripts, forms or active content. External images are left
  in; the window loads them when it shows the message.
- `text` is plain text.
- `message_id` and `in_reply_to` have no angle brackets. `refs` is a space-separated list of
  message ids, also without brackets.
- `uid` is `null` while a local move waits for the server.

## `maild ctl` and `maild add-account`

- `maild ctl '<json>'` or `… | maild ctl` sends one request and prints the response line.
  - The exit status is 0 if `ok` is true, 1 otherwise.
  - The request may be pretty-printed; it is sent as one line.
- `maild ctl '{"cmd":"subscribe"}'` prints the response, then every event, until Ctrl+C.
- `maild add-account <gmail|yandex|imap> <email> [name] [--imap host:port --smtp host:port]`
  reads the password from stdin (trailing newlines are dropped), sends `account_add` and
  prints the response:

  ```sh
  read -rs P; printf %s "$P" | maild add-account gmail me@gmail.com "Max"; unset P
  ```
