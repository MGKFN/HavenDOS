# HavenDOS v0.7.14 — BOOT Chat v1

The first consumer of the multi-socket TCP stack from v0.7.13.

## Why this only exists now

BOOT Chat has been on the roadmap since v0.6 and was blocked the whole time on
one thing: the TCP stack had a single global connection. A chat client has to
hold a socket open for minutes while DNS, the shell and everything else still
work, and that was impossible until v0.7.13 replaced `g_conn` with a real
socket table. Nothing else was ever the obstacle.

## Client — `shell/chat.c`

    chat <nick>@<host>[:<port>]      default port 6700
    chat                             reconnect to the last server

A full-window client with a title bar, scrollback and an input line.

- **200-line scrollback ring**, PgUp/PgDn to move, End to return to the bottom.
  Fixed-size rather than heap-allocated, so a long session cannot fragment the
  heap or fail mid-conversation.
- **Auto-follow only when pinned to the bottom.** A busy room does not yank the
  view away while you are reading back.
- **Long lines wrap** at a word boundary instead of being truncated.
- **Own messages in green**, others in white, joins and parts in cyan.
- **Unknown verbs are shown in grey, not dropped** — a server extension never
  leaves you staring at a silent window wondering if the link died.
- **~20 Hz redraw** rather than one frame per keystroke. A full frame is a
  screen of VGA writes and doing that per key makes typing feel heavy on TCG.
- Everything is polled — a short `tcp_recv()` timeout alternating with a
  non-blocking keyboard read. No IRQ dependency anywhere, per the house rule.
- `/quit` or ESC to leave, `/names` for the member list. Toast plus a
  notification-centre entry on exit.

The v0.7.11 `chat` stub inside `shell.c` has been removed — it advertised port
7700 and "full implementation planned for v0.7.x". New code went in its own
file per the usual rule; `shell.c` only lost the stub and gained a dispatch
line.

## Protocol

Line-based ASCII, `\n` terminated, deliberately trivial.

    client -> server    NICK <nick> | SAY <text> | NAMES | PONG | QUIT
    server -> client    JOIN <nick> | PART <nick> | MSG <nick> <text>
                        NAMES <nick> ... | ERR <text> | PING

The server echoes a speaker's own `SAY` back as a `MSG` rather than the client
echoing locally. That way what you see is what the room saw — if your message
never round-trips, you know it never landed.

## Reference server — `tools/bootchat_server.py`

Standard library only, single file, no dependencies. Threaded rather than
async so it reads the same way the client does: one connection, one loop.

    python3 tools/bootchat_server.py --port 6700

Nick validation and collision rejection, member list, and a PING/PONG idle
timer that drops a client after 3 unanswered pings — which is what stops a
hard-powered-off VM from lingering in the room.

## Verified

Installed to a fresh 8 GB disk, full setup wizard, logged in, then
`chat kyan@10.0.2.2:6700` against the reference server running on the host:

- Title bar `BOOT Chat kyan@10.0.2.2` with `ONLINE`
- `-- connected`, then `-- in room: kyan` from the NAMES reply
- `<kyan> hello from HavenDOS` displayed in green, having round-tripped
  through the server
- Host server log: `join: kyan from 127.0.0.1:49680` then `part: kyan` —
  a clean QUIT, not a dropped socket

## Not verified

- Two or more clients in a room at once.
- Long-session behaviour: scrollback wrap past 200 lines, PING/PONG over the
  full 30-second idle timer, reconnect after a server restart.
- Real hardware.
