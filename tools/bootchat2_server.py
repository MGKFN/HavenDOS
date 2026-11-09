#!/usr/bin/env python3
"""
bootchat2_server.py — BOOT Chat v2: accounts, servers, channels, DMs, history

    python3 bootchat2_server.py                       # 0.0.0.0:6700, ./bootchat.db
    python3 bootchat2_server.py --db D:\\chat\\boot.db --port 6700

Standard library only — sqlite3 and hashlib are both built in, so there is
still nothing to install.

WHAT CHANGED FROM v1

v1 had one room, no identity and nothing on disk. A nickname was just a string
you claimed. DMs and history are meaningless without real identity, so v2 adds
accounts first and builds the rest on top:

    accounts   register / login, PBKDF2-SHA256 password hashing, reconnect
               tokens so a client does not hold a password in memory
    servers    named spaces you create and invite people to, each with channels
    channels   the actual rooms; every server gets #general on creation
    DMs        private one-to-one, stored per user pair
    history    every message persisted in SQLite and backfilled on join

STORAGE

One SQLite file. Back it up by copying it; move it to a VPS later by copying
it. Nothing else in the system holds state.

PROTOCOL — line-based, UTF-8, \\n terminated

Fields are space-separated and the trailing free-text field runs to end of
line, so parsing in C is a handful of strchr() calls. That constraint exists
because HavenDOS has to implement this client too.

    client -> server
        PROTO <version>                 opt in to 2.1 (see below); optional
        REGISTER <user> <pass>          create an account
        LOGIN <user> <pass>             authenticate
        TOKEN <token>                   reconnect without a password
        SERVERS                         list servers I belong to
        CREATE <server>                 create one, I become the owner
        JOINSRV <server>                join an existing server
        CHANNELS <server>               list its channels
        MKCHAN <server> <channel>       add a channel
        JOIN <server> <channel>         switch to it, receive backfill
        SAY <text>                      post to the current channel
        REPLY <id> <text>               post as an answer to <id>          [2.1]
        DM <user> <text>                private message
        DMHIST <user>                   backfill a DM thread
        DMLIST                          who I have threads with
        USERS <server>                  members, for @mention completion  [2.1]
        EDIT <id> <text>                edit a message I wrote            [2.1]
        DEL <id>                        delete a message I wrote          [2.1]
        WHO                             who is online
        PONG / QUIT

    server -> client
        OK <verb> [detail]              command succeeded
        ERR <text>                      command failed
        TOKEN <token>                   after REGISTER or LOGIN
        SERVERLIST <a> <b> ...
        CHANLIST <server> <a> <b> ...
        USERLIST <server> <a> <b> ...                                     [2.1]
        MSG <server> <channel> <user> <ts> <text>                         [2.0]
        MSG <server> <channel> <id> <user> <ts> <ed> <re> <text>          [2.1]
        DMMSG <from> <to> <ts> <text>                                     [2.0]
        DMMSG <id> <from> <to> <ts> <ed> <re> <text>                      [2.1]

      <ed> is 1 when the message has been edited, else 0.
      <re> is the id this message answers, or 0.  Both sit before <text>
      because the trailing field runs to end of line.
        MSGEDIT <id> <ts> <text>                                          [2.1]
        MSGDEL <id>                                                       [2.1]
        HISTEND <kind>                  backfill finished
        JOINED <user> <server> <channel>
        LEFT <user>
        ONLINE <a> <b> ...
        PING

PROTOCOL VERSIONING — why MSG has two shapes

Editing and deleting need a message id on the wire, and the 2.0 MSG line has
nowhere to put one: its trailing field runs to end of line, so a new field
cannot be appended.  Changing MSG outright would have broken every HavenDOS
client already installed, which is the one thing that must not happen.

So a client ASKS for the new shape: send `PROTO 2.1` and the server switches
that connection's message format.  Say nothing and you keep 2.0 exactly as it
was.  The server tracks this per connection, so a 2.0 HavenDOS and a 2.1
browser can sit in the same channel and each sees a line it can parse.

Message ids carry their own table: `c<rowid>` for a channel message, `d<rowid>`
for a DM.  One field, self-describing, and still a single strchr() in C.


SECURITY

Passwords are hashed, never stored or logged in the clear. Everything else is
plaintext over TCP with no transport encryption, because HavenDOS cannot do
TLS for this yet. Run it on your own LAN or over a private network such as
Tailscale. Do not port-forward it to the open internet.
"""

import argparse
import hashlib
import hmac
import os
import socket
import sqlite3
import threading
import time

PING_EVERY = 30.0
PING_LIMIT = 3
MAX_LINE = 2048
BACKFILL = 50          # messages replayed when joining a channel or DM thread
MAX_TEXT = 1500        # longest message body accepted, server-side
POST_RATE = 4.0        # sustained messages per second, per connection
POST_BURST = 10        # …after this many sent back-to-back
EDIT_WINDOW = 15 * 60  # seconds a message stays editable by its author

clients = {}           # sock -> Client
clients_lock = threading.Lock()
db_lock = threading.Lock()
DB = None


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


# ── storage ──────────────────────────────────────────────────────────────

SCHEMA = """
CREATE TABLE IF NOT EXISTS users(
    id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL COLLATE NOCASE,
    salt BLOB NOT NULL, pwhash BLOB NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS tokens(
    token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS servers(
    id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL COLLATE NOCASE,
    owner_id INTEGER NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS members(
    server_id INTEGER NOT NULL, user_id INTEGER NOT NULL,
    PRIMARY KEY(server_id, user_id));
CREATE TABLE IF NOT EXISTS channels(
    id INTEGER PRIMARY KEY, server_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE NOCASE, created INTEGER NOT NULL,
    UNIQUE(server_id, name));
CREATE TABLE IF NOT EXISTS messages(
    id INTEGER PRIMARY KEY, channel_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL, ts INTEGER NOT NULL, text TEXT NOT NULL,
    edited INTEGER NOT NULL DEFAULT 0, reply_to INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS dms(
    id INTEGER PRIMARY KEY, lo_id INTEGER NOT NULL, hi_id INTEGER NOT NULL,
    from_id INTEGER NOT NULL, ts INTEGER NOT NULL, text TEXT NOT NULL,
    edited INTEGER NOT NULL DEFAULT 0, reply_to INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_msg_chan ON messages(channel_id, id);
CREATE INDEX IF NOT EXISTS idx_dm_pair  ON dms(lo_id, hi_id, id);
"""


def db_init(path):
    global DB
    DB = sqlite3.connect(path, check_same_thread=False)
    DB.row_factory = sqlite3.Row
    with db_lock:
        DB.executescript(SCHEMA)
        # CREATE TABLE IF NOT EXISTS will not add a column to a table that
        # already exists, so an existing bootchat.db keeps its old shape and
        # every INSERT naming `edited` fails.  Add it explicitly, once.
        for table in ("messages", "dms"):
            cols = [r["name"] for r in DB.execute("PRAGMA table_info(%s)" % table)]
            for col in ("edited", "reply_to"):
                if col not in cols:
                    DB.execute("ALTER TABLE %s ADD COLUMN %s INTEGER NOT NULL "
                               "DEFAULT 0" % (table, col))
                    log("migrated: %s.%s added" % (table, col))
        DB.commit()
    log("database: %s" % os.path.abspath(path))


def q(sql, args=(), one=False, write=False):
    with db_lock:
        cur = DB.execute(sql, args)
        if write:
            DB.commit()
            return cur.lastrowid
        rows = cur.fetchall()
    return (rows[0] if rows else None) if one else rows


def hash_pw(password, salt=None):
    salt = salt or os.urandom(16)
    h = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, 100_000)
    return salt, h


def valid_name(name, maxlen=23):
    """ASCII only: HavenDOS renders these in fixed-width CP437 cells."""
    if not name or len(name) > maxlen:
        return False
    return all((c.isascii() and c.isalnum()) or c in "-_." for c in name)


# ── connection state ─────────────────────────────────────────────────────

class Client:
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.uid = None
        self.name = None
        self.server = None      # current server name
        self.channel = None     # current channel name
        self.chan_id = None
        self.misses = 0
        # 2.0 until the client asks for more, so an existing HavenDOS build
        # keeps the exact wire format it was written against.
        self.proto = (2, 0)
        # token bucket: POST_BURST messages instantly, then POST_RATE/sec
        self.tokens = float(POST_BURST)
        self.last_post = time.time()

    def proto_at_least(self, major, minor):
        return self.proto >= (major, minor)

    def allow_post(self):
        """Cheap per-connection rate limit. Returns False when over budget."""
        now = time.time()
        self.tokens = min(float(POST_BURST),
                          self.tokens + (now - self.last_post) * POST_RATE)
        self.last_post = now
        if self.tokens < 1.0:
            return False
        self.tokens -= 1.0
        return True

    def send(self, line):
        try:
            self.sock.sendall((line + "\n").encode("utf-8", "replace"))
        except OSError:
            pass

    def ok(self, verb, detail=""):
        self.send(("OK %s %s" % (verb, detail)).rstrip())

    def err(self, text):
        self.send("ERR " + text)


def online_clients():
    with clients_lock:
        return [c for c in clients.values() if c.uid]


def to_channel(chan_id, line, skip=None):
    for c in online_clients():
        if c.chan_id == chan_id and c is not skip:
            c.send(line)


def to_user(uid, line):
    for c in online_clients():
        if c.uid == uid:
            c.send(line)


# ── wire formatting ──────────────────────────────────────────────────────
#
# Each connection gets the shape it asked for.  Everything that emits a
# message goes through these two so the 2.0 and 2.1 forms can never drift
# apart in one code path and not another.

def fmt_msg(c, srv, chan, mid, user, ts, edited, reply_to, text):
    if c.proto_at_least(2, 1):
        return "MSG %s %s c%d %s %d %d %s %s" % (
            srv, chan, mid, user, ts, 1 if edited else 0,
            ("c%d" % reply_to) if reply_to else "0", text)
    return "MSG %s %s %s %d %s" % (srv, chan, user, ts, text)


def fmt_dm(c, mid, frm, to, ts, edited, reply_to, text):
    if c.proto_at_least(2, 1):
        return "DMMSG d%d %s %s %d %d %s %s" % (
            mid, frm, to, ts, 1 if edited else 0,
            ("d%d" % reply_to) if reply_to else "0", text)
    return "DMMSG %s %s %d %s" % (frm, to, ts, text)


def to_channel_msg(chan_id, srv, chan, mid, user, ts, edited, reply_to, text):
    """Broadcast one channel message, per-client format."""
    for c in online_clients():
        if c.chan_id == chan_id:
            c.send(fmt_msg(c, srv, chan, mid, user, ts, edited, reply_to, text))


def to_proto21(uid, line):
    """Send only to that user's 2.1 connections — 2.0 has no verb for it."""
    for c in online_clients():
        if c.uid == uid and c.proto_at_least(2, 1):
            c.send(line)


def to_channel_21(chan_id, line):
    for c in online_clients():
        if c.chan_id == chan_id and c.proto_at_least(2, 1):
            c.send(line)


# ── command handlers ─────────────────────────────────────────────────────

def cmd_register(c, rest):
    parts = rest.split(None, 1)
    if len(parts) != 2:
        return c.err("usage: REGISTER <user> <pass>")
    name, pw = parts[0], parts[1].strip()
    if not valid_name(name):
        return c.err("bad username (letters, digits, - _ . only)")
    if len(pw) < 4:
        return c.err("password too short")
    if q("SELECT id FROM users WHERE name=?", (name,), one=True):
        return c.err("username taken")
    salt, h = hash_pw(pw)
    uid = q("INSERT INTO users(name,salt,pwhash,created) VALUES(?,?,?,?)",
            (name, salt, h, int(time.time())), write=True)
    log("register: %s" % name)
    finish_login(c, uid, name)


def cmd_login(c, rest):
    parts = rest.split(None, 1)
    if len(parts) != 2:
        return c.err("usage: LOGIN <user> <pass>")
    name, pw = parts[0], parts[1].strip()
    row = q("SELECT * FROM users WHERE name=?", (name,), one=True)
    if not row:
        return c.err("no such user")
    _, h = hash_pw(pw, row["salt"])
    # constant-time compare lives in hmac, not hashlib
    if not hmac.compare_digest(h, row["pwhash"]):
        log("failed login: %s from %s" % (name, c.addr[0]))
        return c.err("wrong password")
    finish_login(c, row["id"], row["name"])


def cmd_token(c, rest):
    row = q("SELECT u.id AS id, u.name AS name FROM tokens t "
            "JOIN users u ON u.id=t.user_id WHERE t.token=?",
            (rest.strip(),), one=True)
    if not row:
        return c.err("bad token")
    finish_login(c, row["id"], row["name"], new_token=False)


def finish_login(c, uid, name, new_token=True):
    # one session per account: kick any older connection so the member list
    # cannot show the same person twice
    for other in online_clients():
        if other.uid == uid and other is not c:
            other.send("ERR signed in elsewhere")
            try:
                other.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
    c.uid, c.name = uid, name
    if new_token:
        tok = os.urandom(16).hex()
        q("INSERT INTO tokens(token,user_id,created) VALUES(?,?,?)",
          (tok, uid, int(time.time())), write=True)
        c.send("TOKEN " + tok)
    c.ok("LOGIN", name)
    log("login: %s from %s" % (name, c.addr[0]))
    send_servers(c)


def send_servers(c):
    rows = q("SELECT s.name AS name FROM servers s JOIN members m "
             "ON m.server_id=s.id WHERE m.user_id=? ORDER BY s.name", (c.uid,))
    c.send("SERVERLIST " + " ".join(r["name"] for r in rows))


def cmd_create(c, rest):
    name = rest.strip()
    if not valid_name(name):
        return c.err("bad server name")
    if q("SELECT id FROM servers WHERE name=?", (name,), one=True):
        return c.err("server exists")
    now = int(time.time())
    sid = q("INSERT INTO servers(name,owner_id,created) VALUES(?,?,?)",
            (name, c.uid, now), write=True)
    q("INSERT INTO members(server_id,user_id) VALUES(?,?)", (sid, c.uid), write=True)
    q("INSERT INTO channels(server_id,name,created) VALUES(?,?,?)",
      (sid, "general", now), write=True)
    log("server created: %s by %s" % (name, c.name))
    c.ok("CREATE", name)
    send_servers(c)


def cmd_joinsrv(c, rest):
    name = rest.strip()
    row = q("SELECT id FROM servers WHERE name=?", (name,), one=True)
    if not row:
        return c.err("no such server")
    q("INSERT OR IGNORE INTO members(server_id,user_id) VALUES(?,?)",
      (row["id"], c.uid), write=True)
    c.ok("JOINSRV", name)
    send_servers(c)


def member_of(uid, sid):
    return q("SELECT 1 FROM members WHERE server_id=? AND user_id=?",
             (sid, uid), one=True) is not None


def cmd_channels(c, rest):
    name = rest.strip()
    srv = q("SELECT id FROM servers WHERE name=?", (name,), one=True)
    if not srv or not member_of(c.uid, srv["id"]):
        return c.err("not a member of that server")
    rows = q("SELECT name FROM channels WHERE server_id=? ORDER BY name", (srv["id"],))
    c.send("CHANLIST %s %s" % (name, " ".join(r["name"] for r in rows)))


def cmd_mkchan(c, rest):
    parts = rest.split()
    if len(parts) != 2:
        return c.err("usage: MKCHAN <server> <channel>")
    sname, cname = parts
    if not valid_name(cname):
        return c.err("bad channel name")
    srv = q("SELECT id FROM servers WHERE name=?", (sname,), one=True)
    if not srv or not member_of(c.uid, srv["id"]):
        return c.err("not a member of that server")
    if q("SELECT id FROM channels WHERE server_id=? AND name=?",
         (srv["id"], cname), one=True):
        return c.err("channel exists")
    q("INSERT INTO channels(server_id,name,created) VALUES(?,?,?)",
      (srv["id"], cname, int(time.time())), write=True)
    c.ok("MKCHAN", cname)
    cmd_channels(c, sname)


def cmd_join(c, rest):
    parts = rest.split()
    if len(parts) != 2:
        return c.err("usage: JOIN <server> <channel>")
    sname, cname = parts
    srv = q("SELECT id FROM servers WHERE name=?", (sname,), one=True)
    if not srv or not member_of(c.uid, srv["id"]):
        return c.err("not a member of that server")
    ch = q("SELECT id,name FROM channels WHERE server_id=? AND name=?",
           (srv["id"], cname), one=True)
    if not ch:
        return c.err("no such channel")

    if c.chan_id:
        to_channel(c.chan_id, "LEFT " + c.name, skip=c)
    c.server, c.channel, c.chan_id = sname, ch["name"], ch["id"]
    c.ok("JOIN", "%s %s" % (sname, ch["name"]))

    rows = q("SELECT m.id AS id, m.ts AS ts, m.text AS text, m.edited AS edited, "
             "m.reply_to AS reply_to, u.name AS user FROM messages m "
             "JOIN users u ON u.id=m.user_id WHERE m.channel_id=? "
             "ORDER BY m.id DESC LIMIT ?", (ch["id"], BACKFILL))
    for r in reversed(rows):
        c.send(fmt_msg(c, sname, ch["name"], r["id"], r["user"], r["ts"],
                       r["edited"], r["reply_to"], r["text"]))
    c.send("HISTEND channel")
    to_channel(c.chan_id, "JOINED %s %s %s" % (c.name, sname, ch["name"]), skip=c)


def clean_text(text):
    """Server-side validation. Never trust the client to have done it.

    Control characters are stripped rather than rejected: a stray \\r or an
    ANSI escape pasted from a terminal should not cost someone their message,
    but it must not reach a client either — HavenDOS draws straight into VGA
    cells and a control byte there corrupts the display."""
    text = "".join(ch for ch in text if ch == "\t" or ord(ch) >= 0x20)
    text = text.replace("\t", " ").strip()
    return text[:MAX_TEXT]


def _say(c, text, reply_to=0):
    if not c.chan_id:
        return c.err("join a channel first")
    text = clean_text(text)
    if not text:
        return
    if not c.allow_post():
        return c.err("slow down")
    ts = int(time.time())
    mid = q("INSERT INTO messages(channel_id,user_id,ts,text,edited,reply_to) "
            "VALUES(?,?,?,?,0,?)",
            (c.chan_id, c.uid, ts, text, reply_to), write=True)
    to_channel_msg(c.chan_id, c.server, c.channel, mid, c.name, ts, 0,
                   reply_to, text)


def cmd_say(c, rest):
    _say(c, rest, 0)


def cmd_reply(c, rest):
    """REPLY <id> <text> — same as SAY but records what it answers."""
    parts = rest.split(None, 1)
    if len(parts) != 2:
        return c.err("usage: REPLY <id> <text>")
    table, rid = parse_mid(parts[0])
    if table == "messages":
        row = q("SELECT channel_id FROM messages WHERE id=?", (rid,), one=True)
        if not row or row["channel_id"] != c.chan_id:
            return c.err("no such message here")
        return _say(c, parts[1], rid)
    if table == "dms":
        return cmd_dm_reply(c, rid, parts[1])
    return c.err("bad message id")


def cmd_dm(c, rest):
    parts = rest.split(None, 1)
    if len(parts) != 2:
        return c.err("usage: DM <user> <text>")
    target, text = parts[0], clean_text(parts[1])
    row = q("SELECT id,name FROM users WHERE name=?", (target,), one=True)
    if not row:
        return c.err("no such user")
    if not text:
        return
    if not c.allow_post():
        return c.err("slow down")
    _dm_store(c, row, text, 0)


def _dm_store(c, row, text, reply_to):
    lo, hi = sorted((c.uid, row["id"]))
    ts = int(time.time())
    mid = q("INSERT INTO dms(lo_id,hi_id,from_id,ts,text,edited,reply_to) "
            "VALUES(?,?,?,?,?,0,?)",
            (lo, hi, c.uid, ts, text, reply_to), write=True)
    # Both sides, each in its own protocol shape. The sender gets an echo so
    # what they see is what the thread actually stored.
    for peer in online_clients():
        if peer.uid == row["id"] or peer is c:
            peer.send(fmt_dm(peer, mid, c.name, row["name"], ts, 0,
                             reply_to, text))


def cmd_dm_reply(c, rid, text):
    src = q("SELECT lo_id,hi_id FROM dms WHERE id=?", (rid,), one=True)
    if not src or c.uid not in (src["lo_id"], src["hi_id"]):
        return c.err("no such message")
    other_id = src["hi_id"] if src["lo_id"] == c.uid else src["lo_id"]
    row = q("SELECT id,name FROM users WHERE id=?", (other_id,), one=True)
    if not row:
        return c.err("no such user")
    text = clean_text(text)
    if not text:
        return
    if not c.allow_post():
        return c.err("slow down")
    _dm_store(c, row, text, rid)


def cmd_dmhist(c, rest):
    target = rest.strip()
    row = q("SELECT id,name FROM users WHERE name=?", (target,), one=True)
    if not row:
        return c.err("no such user")
    lo, hi = sorted((c.uid, row["id"]))
    rows = q("SELECT d.id AS id, d.ts AS ts, d.text AS text, d.edited AS edited, "
             "d.reply_to AS reply_to, u.name AS frm FROM dms d "
             "JOIN users u ON u.id=d.from_id WHERE d.lo_id=? AND d.hi_id=? "
             "ORDER BY d.id DESC LIMIT ?", (lo, hi, BACKFILL))
    other = row["name"]
    for r in reversed(rows):
        to_ = other if r["frm"] == c.name else c.name
        c.send(fmt_dm(c, r["id"], r["frm"], to_, r["ts"], r["edited"],
                      r["reply_to"], r["text"]))
    c.send("HISTEND dm " + other)


def cmd_dmlist(c, _rest):
    rows = q("SELECT DISTINCT u.name AS name FROM dms d "
             "JOIN users u ON u.id IN (d.lo_id, d.hi_id) "
             "WHERE (d.lo_id=? OR d.hi_id=?) AND u.id<>? ORDER BY u.name",
             (c.uid, c.uid, c.uid))
    c.send("DMLIST " + " ".join(r["name"] for r in rows))


def cmd_who(c, _rest):
    names = sorted({x.name for x in online_clients()})
    c.send("ONLINE " + " ".join(names))


def cmd_users(c, rest):
    """Members of a server — what @mention completion is built from.

    Deliberately the real users table rather than a second list scraped from
    whoever has spoken recently: a mention has to resolve to an account for a
    notification to ever mean anything."""
    name = rest.strip()
    srv = q("SELECT id FROM servers WHERE name=?", (name,), one=True)
    if not srv or not member_of(c.uid, srv["id"]):
        return c.err("not a member of that server")
    rows = q("SELECT u.name AS name FROM users u JOIN members m "
             "ON m.user_id=u.id WHERE m.server_id=? ORDER BY u.name",
             (srv["id"],))
    c.send("USERLIST %s %s" % (name, " ".join(r["name"] for r in rows)))


def parse_mid(token):
    """'c123' -> ('messages', 123).  Returns (None, None) if malformed."""
    token = token.strip()
    if len(token) < 2 or token[0] not in "cd" or not token[1:].isdigit():
        return None, None
    return ("messages" if token[0] == "c" else "dms"), int(token[1:])


def _owned_row(c, table, rid):
    """Fetch a message only if this user wrote it and it is still editable."""
    col = "user_id" if table == "messages" else "from_id"
    row = q("SELECT * FROM %s WHERE id=?" % table, (rid,), one=True)
    if not row:
        return None, "no such message"
    if row[col] != c.uid:
        return None, "not your message"
    return row, None


def cmd_edit(c, rest):
    parts = rest.split(None, 1)
    if len(parts) != 2:
        return c.err("usage: EDIT <id> <text>")
    table, rid = parse_mid(parts[0])
    if not table:
        return c.err("bad message id")
    text = clean_text(parts[1])
    if not text:
        return c.err("empty message")
    row, why = _owned_row(c, table, rid)
    if why:
        return c.err(why)
    if int(time.time()) - row["ts"] > EDIT_WINDOW:
        return c.err("too old to edit")
    q("UPDATE %s SET text=?, edited=1 WHERE id=?" % table,
      (text, rid), write=True)
    tok = ("c" if table == "messages" else "d") + str(rid)
    line = "MSGEDIT %s %d %s" % (tok, row["ts"], text)
    if table == "messages":
        to_channel_21(row["channel_id"], line)
    else:
        to_proto21(row["lo_id"], line)
        to_proto21(row["hi_id"], line)
    c.ok("EDIT", tok)


def cmd_del(c, rest):
    table, rid = parse_mid(rest)
    if not table:
        return c.err("bad message id")
    row, why = _owned_row(c, table, rid)
    if why:
        return c.err(why)
    q("DELETE FROM %s WHERE id=?" % table, (rid,), write=True)
    tok = ("c" if table == "messages" else "d") + str(rid)
    line = "MSGDEL " + tok
    if table == "messages":
        to_channel_21(row["channel_id"], line)
    else:
        to_proto21(row["lo_id"], line)
        to_proto21(row["hi_id"], line)
    c.ok("DEL", tok)


HANDLERS = {
    "SERVERS":  lambda c, r: send_servers(c),
    "CREATE":   cmd_create,
    "JOINSRV":  cmd_joinsrv,
    "CHANNELS": cmd_channels,
    "MKCHAN":   cmd_mkchan,
    "JOIN":     cmd_join,
    "SAY":      cmd_say,
    "DM":       cmd_dm,
    "DMHIST":   cmd_dmhist,
    "DMLIST":   cmd_dmlist,
    "WHO":      cmd_who,
    "USERS":    cmd_users,
    "REPLY":    cmd_reply,
    "EDIT":     cmd_edit,
    "DEL":      cmd_del,
}


# ── connection loop ──────────────────────────────────────────────────────

def handle(sock, addr):
    c = Client(sock, addr)
    with clients_lock:
        clients[sock] = c
    buf = b""
    try:
        while True:
            chunk = sock.recv(2048)
            if not chunk:
                break
            buf += chunk
            if len(buf) > MAX_LINE * 8:
                c.err("flood")
                break
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")[:MAX_LINE]
                if not line:
                    continue
                verb, _, rest = line.partition(" ")
                verb = verb.upper()

                if verb == "PONG":
                    c.misses = 0
                    continue
                if verb == "QUIT":
                    return
                if verb == "PROTO":
                    # Pre-auth on purpose: a client should be able to settle
                    # the wire format before it sends a password.
                    want = rest.strip().split(".")
                    try:
                        ver = (int(want[0]), int(want[1]) if len(want) > 1 else 0)
                    except (ValueError, IndexError):
                        c.err("bad version"); continue
                    c.proto = min(ver, (2, 1))     # never above what we speak
                    c.ok("PROTO", "%d.%d" % c.proto)
                    continue
                if verb == "REGISTER":
                    cmd_register(c, rest); continue
                if verb == "LOGIN":
                    cmd_login(c, rest); continue
                if verb == "TOKEN":
                    cmd_token(c, rest); continue
                if not c.uid:
                    c.err("log in first"); continue

                fn = HANDLERS.get(verb)
                if fn:
                    try:
                        fn(c, rest)
                    except sqlite3.Error as e:
                        c.err("database error: %s" % e)
                    except Exception as e:          # noqa: BLE001
                        # A bug in one handler must not drop the session --
                        # that is exactly how the hashlib/hmac slip presented:
                        # REGISTER worked, LOGIN silently closed the socket.
                        log("handler error in %s: %r" % (verb, e))
                        c.err("internal error in " + verb)
                else:
                    c.err("unknown verb " + verb)
    except (OSError, UnicodeError):
        pass
    finally:
        with clients_lock:
            clients.pop(sock, None)
        if c.uid:
            if c.chan_id:
                to_channel(c.chan_id, "LEFT " + c.name)
            log("disconnect: %s" % c.name)
        try:
            sock.close()
        except OSError:
            pass


def pinger():
    while True:
        time.sleep(PING_EVERY)
        for c in list(online_clients()):
            if c.misses >= PING_LIMIT:
                log("timeout: %s" % c.name)
                try:
                    c.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                continue
            c.misses += 1
            c.send("PING")


def main():
    ap = argparse.ArgumentParser(description="BOOT Chat v2 server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=6700)
    ap.add_argument("--db", default="bootchat.db")
    a = ap.parse_args()

    db_init(a.db)
    threading.Thread(target=pinger, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(32)
    log("BOOT Chat v2 on %s:%d" % (a.host, a.port))
    try:
        while True:
            s, addr = srv.accept()
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=handle, args=(s, addr), daemon=True).start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
