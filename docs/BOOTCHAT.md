# BootChat

BootChat is the communication system of the HavenDOS ecosystem. One server,
three clients, one protocol.

```
                       ┌──────────────────────────┐
                       │   bootchat2_server.py    │
                       │   accounts · servers     │
                       │   channels · DMs · history│
                       │   SQLite: bootchat.db    │
                       └────────────┬─────────────┘
                          raw TCP 6700, line protocol
             ┌──────────────────────┼──────────────────────┐
             │                      │                      │
   ┌─────────┴────────┐   ┌─────────┴────────┐   ┌─────────┴────────┐
   │ bootchat2_web.py │   │ bootchat2_gui.py │   │  shell/chat.c    │
   │  HTTP bridge     │   │  tkinter desktop │   │  HavenDOS client │
   └─────────┬────────┘   └──────────────────┘   └──────────────────┘
             │ HTTP(S), long poll                     proto 2.0
   ┌─────────┴────────┐
   │     browser      │  proto 2.1
   └──────────────────┘
```

The server is the only thing that holds state. Everything else is a client,
including the web bridge — it opens an ordinary TCP connection per browser
session, so the server cannot tell a phone from a desktop.

---

## Why there are two wire formats

Editing and deleting a message need an id on the wire. The 2.0 `MSG` line has
nowhere to put one: its trailing field runs to end of line, deliberately, so
that parsing it in C is a handful of `strchr()` calls. Appending a field is
not possible; inserting one would have broken every HavenDOS client already
installed.

So a client **asks for** the newer shape:

```
client → PROTO 2.1
server → OK PROTO 2.1
```

A connection that never sends `PROTO` gets 2.0 exactly as it always was. The
server records the level per connection, so a 2.0 HavenDOS and a 2.1 browser
can sit in the same channel and each receives a line it can parse.

Message ids carry their own table: `c<rowid>` for a channel message,
`d<rowid>` for a DM. One field, self-describing, still trivially parseable.

---

## Protocol

Line-based, UTF-8, `\n` terminated. Fields are space-separated and the
trailing free-text field runs to end of line.

### Client → server

| verb | meaning | proto |
|---|---|---|
| `PROTO <version>` | request a wire format | any |
| `REGISTER <user> <pass>` | create an account | 2.0 |
| `LOGIN <user> <pass>` | authenticate | 2.0 |
| `TOKEN <token>` | reconnect without a password | 2.0 |
| `SERVERS` | list servers I belong to | 2.0 |
| `CREATE <server>` | create one, I become the owner | 2.0 |
| `JOINSRV <server>` | join an existing server | 2.0 |
| `CHANNELS <server>` | list its channels | 2.0 |
| `MKCHAN <server> <channel>` | add a channel | 2.0 |
| `JOIN <server> <channel>` | switch to it, receive backfill | 2.0 |
| `SAY <text>` | post to the current channel | 2.0 |
| `DM <user> <text>` | private message | 2.0 |
| `DMHIST <user>` | backfill a DM thread | 2.0 |
| `DMLIST` | who I have threads with | 2.0 |
| `WHO` | who is online | 2.0 |
| `USERS <server>` | members, for mention completion | 2.1 |
| `REPLY <id> <text>` | post as an answer to `<id>` | 2.1 |
| `EDIT <id> <text>` | edit a message I wrote | 2.1 |
| `DEL <id>` | delete a message I wrote | 2.1 |
| `PONG` / `QUIT` | keepalive / disconnect | 2.0 |

### Server → client

```
OK <verb> [detail]
ERR <text>
TOKEN <token>
SERVERLIST <a> <b> ...
CHANLIST <server> <a> <b> ...
USERLIST <server> <a> <b> ...                            [2.1]
MSG <server> <channel> <user> <ts> <text>                [2.0]
MSG <server> <channel> <id> <user> <ts> <ed> <re> <text> [2.1]
DMMSG <from> <to> <ts> <text>                            [2.0]
DMMSG <id> <from> <to> <ts> <ed> <re> <text>             [2.1]
MSGEDIT <id> <ts> <text>                                 [2.1]
MSGDEL <id>                                              [2.1]
HISTEND <kind>
JOINED <user> <server> <channel>
LEFT <user>
ONLINE <a> <b> ...
PING
```

`<ed>` is `1` when the message has been edited, else `0`.
`<re>` is the id this message answers, or `0`. Both sit before `<text>`
because the trailing field runs to end of line.

### Authentication

`REGISTER` or `LOGIN` returns a `TOKEN`. Clients store the token and replay
it with `TOKEN` on reconnect, so no client holds a password after sign-in.
Passwords are PBKDF2-SHA256 with a per-user salt and are compared with
`hmac.compare_digest`. One session per account: logging in elsewhere
disconnects the older connection.

### Limits and validation

Enforced on the server, never trusted to the client:

| limit | value |
|---|---|
| message length | 1500 characters, truncated not rejected |
| line length | 2048 bytes |
| send rate | 4/s sustained, burst of 10, per connection |
| edit window | 15 minutes from posting |
| username | ≤23 chars, `A-Za-z0-9`, `-`, `_`, `.` |
| backfill | last 50 messages per channel or thread |

Control characters are stripped from message text before broadcast. This is
not cosmetic: HavenDOS writes message bytes straight into VGA cells, where a
stray escape byte corrupts the display.

---

## HTTP bridge API

The browser never speaks the chat protocol directly. It posts verbs to the
bridge and reads back raw server lines. The bridge parses nothing — a
protocol change is usually a page-only edit.

| endpoint | method | purpose |
|---|---|---|
| `/api/open` | POST | open a session; returns `{ok, sid}` |
| `/api/cmd` | POST | `{sid, line}` → send one protocol line |
| `/api/poll` | GET | `?sid=&since=` → `{ok, alive, lines[], cursor}` |
| `/api/quit` | POST | `{sid}` → close the session |
| `/api/health` | GET | `{ok, sessions, proto}` |

`/api/open` takes **no host parameter**. The chat server address comes from
configuration only; accepting it from the client would turn the bridge into
an open relay to any address a caller names.

### Long polling

`/api/poll` blocks until a line arrives or `POLL_WAIT` (25s) passes. The
previous design polled on a fixed 1s timer, costing one request per second
per open tab whether or not anything had happened and adding up to a second
of latency to every message.

**Consequence worth knowing:** there is always a request in flight, so
"wait for network idle" never fires against BootChat. Browser automation and
test harnesses must wait on `load` or `domcontentloaded` instead.

There is no WebSocket in this design, and therefore no WSS to configure.
Long polling was kept deliberately: standard library only, survives proxies
that mishandle upgrade headers, and at this scale costs about the same. If
that ever changes, the page's transport is three functions.

---

## Configuration

Nothing about where the server lives belongs in the page. Command-line flags
override the environment; both override the defaults.

| variable | default | purpose |
|---|---|---|
| `BOOTCHAT_PUBLIC_URL` | — | public origin, e.g. `https://boot.chat` |
| `BOOTCHAT_PUBLIC_NAME` | host of `PUBLIC_URL` | what the sign-in screen shows |
| `BOOTCHAT_API_BASE` | *(empty)* | prefix if the API is mounted elsewhere |
| `BOOTCHAT_CHAT_HOST` | `127.0.0.1` | chat server host |
| `BOOTCHAT_CHAT_PORT` | `6700` | chat server port |
| `BOOTCHAT_BIND` | `0.0.0.0` | bind address |
| `BOOTCHAT_PORT` | `8080` | HTTP port |
| `BOOTCHAT_BEHIND_PROXY` | unset | set when nginx terminates TLS in front |

---

## Local development

```
python3 tools/bootchat2_server.py                 # 0.0.0.0:6700, ./bootchat.db
python3 tools/bootchat2_web.py                    # web on 8080 → chat 127.0.0.1:6700
python3 tools/bootchat2_gui.py --host 127.0.0.1   # desktop client
```

Nothing needs `pip install` — `sqlite3`, `hashlib` and `tkinter` all ship
with Python on Windows.

From HavenDOS:

```
chat <nick>@<host>[:<port>]        # default port 6700
```

Under QEMU user-mode networking the guest is `10.0.2.15` and **the host is
always `10.0.2.2`** — not `0.0.0.0`, which is only the server's listen
wildcard. From Windows itself the server is on `127.0.0.1`.

---

## Production deployment

```
                    Internet
                       │
                   boot.chat            DNS
                       │
                  ┌────┴────┐
                  │  nginx  │           TLS terminates here
                  └────┬────┘
              ┌────────┴────────┐
              │ bootchat2_web.py│        127.0.0.1:8080
              └────────┬────────┘
              ┌────────┴────────┐
              │bootchat2_server │        127.0.0.1:6700   ← never exposed
              └─────────────────┘
```

```nginx
server {
    listen 443 ssl http2;
    server_name boot.chat;

    ssl_certificate     /etc/letsencrypt/live/boot.chat/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/boot.chat/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Proto $scheme;

        # Long polling holds a request open for up to 25s. Without this
        # nginx closes it at 60s by default and the client reconnects
        # more often than it needs to; below ~30s it would cut every poll.
        proxy_read_timeout 120s;
        proxy_buffering off;
    }
}
```

```
BOOTCHAT_PUBLIC_URL=https://boot.chat \
BOOTCHAT_BEHIND_PROXY=1 \
BOOTCHAT_BIND=127.0.0.1 \
python3 tools/bootchat2_web.py
```

Bind the web bridge to `127.0.0.1` so only nginx can reach it, and never
port-forward 6700.

---

## HTTPS and TLS — what is and is not true

This is the part it would be easy to overstate, so it is set out plainly.

**The browser side is ready for TLS.** Put nginx or Cloudflare in front, set
`BOOTCHAT_PUBLIC_URL` to the `https://` origin, and the page is served over
HTTPS with same-origin API calls. That is also what makes the service worker
register, and what Web Push would require.

**The HavenDOS side is not.** `shell/chat.c` speaks the protocol as raw TCP
on port 6700 with no transport encryption, because HavenDOS cannot do TLS for
this yet. That is a real capability limit, not an oversight:

* Cloudflare Tunnel carries HTTP, not raw TCP, so it cannot serve the
  HavenDOS client at all.
* Run that leg on a LAN, or over Tailscale, which gives a stable name and
  works from anywhere.

Passwords travel from the browser to the bridge and on to the chat server.
Over HTTPS the first leg is encrypted; the bridge-to-server leg is not, so
keep them on the same host or a private network.

| | browser | HavenDOS |
|---|---|---|
| transport | HTTP(S) to the bridge | raw TCP :6700 |
| encryption | TLS via reverse proxy | **none** |
| safe over the internet | yes, behind TLS | **no** — LAN or Tailscale only |
| protocol level | 2.1 | 2.0 |

---

## HavenDOS integration

What exists today, with nothing invented:

* `shell/chat.c` is a full client: accounts, servers, channels, DMs, history,
  a sidebar, and a scrollback. Launched as `chat <nick>@<host>`.
* It rides the multi-socket TCP stack added in v0.7.13.
* UTF-8 on the wire in both directions. Incoming text is converted to CP437
  on receipt; accented letters map to real CP437 glyphs generated at build
  time by `tools/make_font_ext.py`.
* Emoji arriving from a phone are spelled out as descriptions — `[grin]`,
  `[heart]`, `[thumbs up]` — because a VGA text cell is one byte with 256
  glyphs and real emoji are impossible. Outgoing, `:shortcode:` expands to
  real UTF-8 so phones see the actual emoji.
* Addressing uses a `hosts.txt` on the HavenDOS filesystem, checked before
  the network, so it costs no DNS round trip and works offline.

It stays on protocol 2.0 and needs no change for this release. Editing and
deleting are browser-side for now; the verbs are in the server whenever the
C client wants them.

### Adding 2.1 to the C client later

1. Send `PROTO 2.1` immediately after connecting, before `LOGIN`.
2. `MSG` gains three leading fields before the text: `<id> … <ed> <re>`.
   `field()` already handles this — add three more calls.
3. Handle `MSGEDIT` (find by id, replace text) and `MSGDEL` (find by id,
   remove). The scrollback would need to keep ids alongside lines, which it
   does not today.

Until then the server keeps sending it 2.0, and nothing breaks.

---

## Operations

**Backup** is copying `bootchat.db`. Nothing else holds state. Moving to a
VPS is copying that file and pointing `--chat-host` at the new address.

**Schema migration** runs automatically at startup: missing `edited` and
`reply_to` columns are added to an existing database. `CREATE TABLE IF NOT
EXISTS` does not add columns to a table that already exists, which is why
this is explicit.

**Windows packaging** — nothing needs installing; the `.py` file is the app.
For a standalone executable:

```
pip install pyinstaller
pyinstaller --onefile --windowed --name "BOOT Chat" tools/bootchat2_gui.py
```

---

## Still open

* **Web Push notifications.** iOS 16.4+ supports them for home-screen PWAs,
  but they require HTTPS, a service worker and VAPID keys — so a real domain
  or Tailscale first. Not implemented.
* **TLS from HavenDOS** to the chat server.
* **Pinned-CA store.** Certificates are parsed safely but not verified
  against a trust anchor.
* **2.1 in the C client**, as above.
* **Clicking a channel in the HavenDOS chat sidebar is unverified.** The
  cursor is live and the coordinate maths reads correctly, but no scripted
  click has ever landed on a target: QEMU's `mouse_move` is relative for a
  PS/2 mouse and passes through the guest's own acceleration. Keyboard
  navigation (Tab / arrows / Enter) is verified.
