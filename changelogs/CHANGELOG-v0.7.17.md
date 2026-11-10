# HavenDOS v0.7.17 — sidebar, mouse, hostnames, installable web app

## The HavenDOS client had no sidebar

v0.7.16 shipped the v2 protocol but kept v1's single scrolling pane. Servers,
channels and DMs existed on the wire and were invisible on screen — you
navigated a tree you could not see by typing `/join` and hoping. The web and
desktop clients both had a sidebar; this one did not, and it was the worst of
the three to use.

It now matches the other two:

```
+----------------+-------------------------------------+
| BOOT Chat      | yan  techhaven/#dev          ONLINE |
| SERVERS        |-------------------------------------|
|  techhaven     | messages                            |
|   # dev        |                                     |
|   # general    |                                     |
|   # random   * |   <- unread marker                  |
| DIRECT         |                                     |
|   @ bob        |                                     |
+----------------+-------------------------------------+
| > type here                                          |
```

- **TAB** moves focus between the sidebar and the message line
- **UP/DOWN** move the selection, **ENTER** opens it
- **Click** anywhere in the sidebar does the same
- **PGUP/PGDN** scroll messages, **END** returns to the bottom

The slash commands all still work, because creating things still needs a name
typed in. The sidebar covers getting *around*, which is what was painful.

A message arriving in a room you are not looking at now marks that row with a
yellow `*` instead of being silently dropped. Unread flags survive a channel
list refresh — otherwise a routine `CHANLIST` would wipe the marks that tell
you where new traffic landed.

## Mouse acceleration was asymmetric — `drivers/mouse.c`

Found while working out why a scripted click kept missing. The boost test was:

```c
if(raw_accum < 0 ? -raw_accum : raw_accum > 6 * divisor)
```

`>` binds tighter than `?:`, so that parses as:

```c
if( raw_accum < 0 ? (-raw_accum) : (raw_accum > 6*divisor) )
```

For a **negative** accumulator the condition is `-raw_accum` — a positive
number, so always true. Every leftward and upward movement got the 2× boost
regardless of speed, while rightward and downward movement was boosted only
when genuinely fast.

The pointer moved twice as far left as right for the same hand motion. That
reads as the mouse "drifting" or "fighting you", and it is why precise
clicking has always been awkward — in the desktop as much as in chat, since
every pointer movement in the OS goes through this function.

## Hostnames instead of IP addresses — `net/dns.c`

`chat yan@10.0.2.2:6700` tells you nothing about where you are connecting.
There is now a plain-text `hosts.txt` on the HavenDOS filesystem:

```
# name            address
bootchat          10.0.2.2
laptop            192.168.2.20
```

Checked **before** the network, so a name here costs no DNS round trip and
works with the network down. Case-insensitive. `nslookup -flush` re-reads it
so an edit takes effect without a reboot.

A starter file is written on first boot mapping `bootchat` and `host` to
10.0.2.2, so `chat yan@bootchat:6700` works out of the box.

This is deliberately not mDNS. Real `.local` resolution needs a multicast
responder and a second protocol; a hosts file is fifty lines and solves the
actual problem, which is wanting to type a name.

## The web client installs as an app

`bootchat2_web.py` is now a PWA. On iOS: **Share → Add to Home Screen** gives
a real app icon, no Safari chrome, and its own app switcher entry.

- Manifest, 192/512/maskable icons, and an `apple-touch-icon` — iOS does not
  read the manifest for Add to Home Screen, it reads the `apple-*` meta tags,
  so both are present
- A service worker caches the app shell (never `/api/*` — stale chat is worse
  than no chat). It only registers over HTTPS or localhost; on a plain-HTTP
  LAN address registration fails, which is fine: the standalone app comes
  from the meta tags, not the worker
- **The reconnect token is remembered**, so a returning installed app goes
  straight to the chat. iOS unloads a backgrounded PWA aggressively and
  without this every return was a password prompt. A rejected token drops
  cleanly back to the login screen; **Out** signs out and clears it
- Icons are embedded as base64 (~10 KB) so the client stays one file with
  nothing to install. `tools/make_pwa_icons.py` regenerates them
- A one-time install hint on iOS Safari, since there is no
  `beforeinstallprompt` on that platform
- UI: unread dots in the sidebar, consecutive messages from one person
  collapse into a block, connection status pill

## Verified

- Clean two-pass build, no new warnings; installs, boot chain intact
- `chat yan@bootchat:6700` — **resolved through hosts.txt**, registered,
  logged in, created a server; server log confirms each step
- Sidebar renders the full tree: `SERVERS / techhaven / #dev / #general /
  #random / DIRECT / @bob`, with the unread `*` on a DM that arrived while
  looking elsewhere
- Mouse cursor is live inside the chat window
- Web client: all PWA routes serve correctly (manifest `display: standalone`,
  3 icons, real PNG bytes, all five meta tags present), protocol round-trips,
  token resume works, bad token rejected
- `make_pwa_icons.py --write` reproduces the embedded icons byte-for-byte

## Not verified

- **Clicking a channel in the HavenDOS sidebar.** The cursor is live and the
  coordinate maths reads correctly, but I never landed a scripted click on a
  target: QEMU's `mouse_move` is *relative* for a PS/2 mouse and goes through
  the guest's own acceleration, so driving it precisely from the monitor is
  awkward. Keyboard navigation is verified; the mouse path is not.
- Two HavenDOS clients in one room
- The PWA on a real iPhone
- Real hardware
