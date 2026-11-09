#!/usr/bin/env python3
"""End-to-end test of the BOOT Chat v2.1 server over a real socket.

Checks the things that actually matter for this change:
  - a 2.0 client still sees the exact wire format it was written against
  - a 2.1 client gets ids and the edited flag
  - both can sit in one channel simultaneously
  - EDIT/DEL enforce ownership
  - the sqlite migration adds `edited` to a pre-existing database
"""
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "tree", "havendos-v0.7.17-source",
                      "tools", "bootchat2_server.py")
PORT = 6789
fails = 0
total = 0


def ck(what, cond, detail=""):
    global fails, total
    total += 1
    if cond:
        print("  ok    %s" % what)
    else:
        fails += 1
        print("  FAIL  %s %s" % (what, detail))


class C:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.s.settimeout(2.0)
        self.buf = b""

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def lines(self, upto=0.6):
        """Drain whatever arrives within a short window."""
        out, end = [], time.time() + upto
        while time.time() < end:
            try:
                self.s.settimeout(max(0.05, end - time.time()))
                chunk = self.s.recv(4096)
                if not chunk:
                    break
                self.buf += chunk
            except socket.timeout:
                break
            except OSError:
                break
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                t = raw.decode("utf-8", "replace").strip()
                if t and t != "PING":
                    out.append(t)
        return out

    def first(self, verb, upto=0.6):
        for l in self.lines(upto):
            if l.split(" ")[0] == verb:
                return l
        return None

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "test.db")

    # Pre-create a database with the OLD schema (no `edited`) so the
    # migration path is what actually runs.
    old = sqlite3.connect(db)
    old.executescript("""
      CREATE TABLE messages(id INTEGER PRIMARY KEY, channel_id INTEGER NOT NULL,
        user_id INTEGER NOT NULL, ts INTEGER NOT NULL, text TEXT NOT NULL);
      CREATE TABLE dms(id INTEGER PRIMARY KEY, lo_id INTEGER NOT NULL,
        hi_id INTEGER NOT NULL, from_id INTEGER NOT NULL, ts INTEGER NOT NULL,
        text TEXT NOT NULL);
    """)
    old.commit(); old.close()
    print("pre-existing DB created with the OLD schema (no `edited`)")

    proc = subprocess.Popen([sys.executable, SERVER, "--port", str(PORT),
                             "--db", db],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    time.sleep(1.2)
    if proc.poll() is not None:
        print("server died:\n" + proc.stdout.read())
        return 1

    try:
        cols = [r[1] for r in sqlite3.connect(db).execute(
            "PRAGMA table_info(messages)")]
        ck("migration added messages.edited to an existing DB", "edited" in cols,
           cols)

        print("\n2.0 client (an existing HavenDOS)")
        a = C()
        a.send("REGISTER alice pw1234")
        ck("register succeeds", a.first("OK") is not None)
        a.send("CREATE techhaven")
        a.lines()
        a.send("JOIN techhaven general")
        a.lines()

        print("\n2.1 client (the new web client)")
        b = C()
        b.send("PROTO 2.1")
        ok = b.first("OK")
        ck("PROTO negotiated", ok is not None and "2.1" in ok, ok)
        b.send("REGISTER bob pw1234")
        b.lines()
        b.send("JOINSRV techhaven")
        b.lines()
        b.send("JOIN techhaven general")
        b.lines()

        print("\nsame channel, two wire formats")
        b.send("SAY hello from the browser")
        amsg = a.first("MSG")
        bmsg = b.first("MSG")
        # 2.0: MSG <srv> <chan> <user> <ts> <text>
        ck("2.0 client sees the legacy 5-field form",
           amsg is not None and amsg.split(" ")[3] == "bob", amsg)
        # 2.1: MSG <srv> <chan> <id> <user> <ts> <ed> <text>
        ck("2.1 client sees an id and the edited flag",
           bmsg is not None and bmsg.split(" ")[3].startswith("c")
           and bmsg.split(" ")[4] == "bob", bmsg)
        mid = bmsg.split(" ")[3] if bmsg else "c1"

        print("\nedit and delete")
        b.send("EDIT %s hello from the browser (fixed)" % mid)
        ed = b.first("MSGEDIT")
        ck("author can edit; MSGEDIT broadcast", ed is not None, ed)
        ck("2.0 client is NOT sent a verb it cannot parse",
           a.first("MSGEDIT", 0.4) is None)

        a.send("EDIT %s hijacked" % mid)
        err = a.first("ERR")
        ck("a different user cannot edit it", err is not None
           and "not your message" in err, err)

        a.send("DEL %s" % mid)
        err = a.first("ERR")
        ck("a different user cannot delete it", err is not None, err)

        b.send("DEL %s" % mid)
        ck("author can delete", b.first("MSGDEL") is not None)

        print("\nmentions source")
        b.send("USERS techhaven")
        ul = b.first("USERLIST")
        ck("USERLIST returns real accounts",
           ul is not None and "alice" in ul and "bob" in ul, ul)

        print("\nvalidation and limits")
        b.send("SAY " + "x" * 4000)
        m = b.first("MSG")
        body = m.split(" ", 8)[8] if m and len(m.split(" ", 8)) > 8 else ""
        ck("over-long message is truncated server-side, not rejected",
           0 < len(body) <= 1500, len(body))

        b.send("SAY bad\x07bell\x1b[31mescape")
        m = b.first("MSG")
        ck("control characters stripped before broadcast",
           m is not None and "\x07" not in m and "\x1b" not in m, repr(m))

        flood = C(); flood.send("PROTO 2.1"); flood.lines(0.3)
        flood.send("LOGIN bob pw1234"); flood.lines(0.5)
        flood.send("JOIN techhaven general"); flood.lines(0.5)
        for i in range(40):
            flood.send("SAY flood %d" % i)
        errs = [l for l in flood.lines(1.5) if l.startswith("ERR")]
        ck("rate limiter engages on a flood", len(errs) > 0, len(errs))
        flood.close()

        print("\nbackfill carries ids")
        c2 = C(); c2.send("PROTO 2.1"); c2.lines(0.3)
        c2.send("LOGIN alice pw1234"); c2.lines(0.5)
        c2.send("JOIN techhaven general")
        hist = [l for l in c2.lines(1.0) if l.startswith("MSG ")]
        ck("history replays in 2.1 form with ids",
           len(hist) > 0 and hist[0].split(" ")[3].startswith("c"),
           hist[0] if hist else None)
        c2.close()

        a.close(); b.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("\n%d/%d passed" % (total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
