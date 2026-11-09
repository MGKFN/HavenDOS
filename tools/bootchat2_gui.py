#!/usr/bin/env python3
"""
bootchat2_gui.py — BOOT Chat v2 desktop client (Windows / macOS / Linux)

    python3 bootchat2_gui.py
    python3 bootchat2_gui.py --host 192.168.2.20 --port 6700

tkinter only, so on Windows it runs with the Python you already have — no pip,
no install. Double-clicking the file works too.

LAYOUT

    +----------------+--------------------------------------+
    | servers        |  #channel                            |
    |   techhaven    |  messages                            |
    |   channels     |                                      |
    |     general    |                                      |
    |     dev        |                                      |
    | direct msgs    |                                      |
    |   bob          |                                      |
    |   alice        |  [ type here                ] [Send] |
    +----------------+--------------------------------------+

The sidebar is one tree: servers with their channels, then DM threads. Click
anything to switch to it. History arrives from the server on every switch, so
scrollback is the same on any device you sign in from.

NOTES

The socket lives on a reader thread; tkinter is not thread-safe, so incoming
lines go on a queue that the UI drains on a timer. That is the standard
pattern and avoids the intermittent crashes you get from touching widgets off
the main thread.
"""

import argparse
import queue
import socket
import threading
import tkinter as tk
from tkinter import ttk, messagebox
import time

BG      = "#0f1115"
PANEL   = "#171a21"
FG      = "#d8dae0"
ACCENT  = "#4ea1ff"
MUTED   = "#7a8290"
OWN     = "#6ee787"
ERRCOL  = "#ff7b72"


class Net:
    """Socket plus a reader thread. Lines land on .q for the UI to drain."""

    def __init__(self):
        self.sock = None
        self.q = queue.Queue()
        self.alive = False

    def connect(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=6)
        self.sock.settimeout(None)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.alive = True
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        try:
            while self.alive:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if line == "PING":
                        self.send("PONG")
                    elif line:
                        self.q.put(line)
        except OSError:
            pass
        finally:
            self.alive = False
            self.q.put("__DISCONNECTED__")

    def send(self, line):
        if not self.alive:
            return False
        try:
            self.sock.sendall((line + "\n").encode("utf-8", "replace"))
            return True
        except OSError:
            self.alive = False
            return False

    def close(self):
        self.alive = False
        try:
            self.sock.sendall(b"QUIT\n")
            self.sock.close()
        except (OSError, AttributeError):
            pass


class App:
    def __init__(self, root, host, port):
        self.root = root
        self.host, self.port = host, port
        self.net = Net()
        self.me = None
        self.view = None          # ("chan", server, channel) or ("dm", user)
        self.servers = []
        self.channels = {}        # server -> [channel]
        self.dms = []
        self.pending_join = None

        root.title("BOOT Chat")
        root.geometry("900x600")
        root.configure(bg=BG)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self._build_login()

    # ── login ────────────────────────────────────────────────────────
    def _build_login(self):
        self.login = tk.Frame(self.root, bg=BG)
        self.login.place(relx=.5, rely=.5, anchor="center")
        tk.Label(self.login, text="BOOT Chat", bg=BG, fg=ACCENT,
                 font=("Segoe UI", 22, "bold")).grid(row=0, column=0, columnspan=2, pady=(0, 4))
        tk.Label(self.login, text="TechHaven Studios", bg=BG, fg=MUTED,
                 font=("Segoe UI", 9)).grid(row=1, column=0, columnspan=2, pady=(0, 18))

        self.e_host = self._field("Server", 2, self.host)
        self.e_port = self._field("Port", 3, str(self.port))
        self.e_user = self._field("Username", 4, "")
        self.e_pass = self._field("Password", 5, "", show="*")

        btns = tk.Frame(self.login, bg=BG)
        btns.grid(row=6, column=0, columnspan=2, pady=(16, 6))
        tk.Button(btns, text="Sign in", width=12, command=lambda: self.go("LOGIN"),
                  bg=ACCENT, fg="#06131f", relief="flat",
                  font=("Segoe UI", 10, "bold")).pack(side="left", padx=4)
        tk.Button(btns, text="Create account", width=14, command=lambda: self.go("REGISTER"),
                  bg=PANEL, fg=FG, relief="flat").pack(side="left", padx=4)

        self.l_err = tk.Label(self.login, text="", bg=BG, fg=ERRCOL, font=("Segoe UI", 9))
        self.l_err.grid(row=7, column=0, columnspan=2)
        self.e_user.focus_set()
        self.root.bind("<Return>", lambda _e: self.go("LOGIN"))

    def _field(self, label, row, value, show=None):
        tk.Label(self.login, text=label, bg=BG, fg=MUTED,
                 font=("Segoe UI", 9)).grid(row=row, column=0, sticky="e", padx=(0, 8), pady=3)
        e = tk.Entry(self.login, bg=PANEL, fg=FG, insertbackground=FG,
                     relief="flat", width=26, show=show, font=("Segoe UI", 10))
        e.grid(row=row, column=1, pady=3, ipady=4)
        e.insert(0, value)
        return e

    def go(self, verb):
        user = self.e_user.get().strip()
        pw = self.e_pass.get()
        if not user or not pw:
            self.l_err.config(text="username and password required"); return
        try:
            port = int(self.e_port.get())
        except ValueError:
            self.l_err.config(text="port must be a number"); return
        self.l_err.config(text="connecting...")
        self.root.update_idletasks()
        try:
            self.net.connect(self.e_host.get().strip(), port)
        except OSError as e:
            self.l_err.config(text="cannot reach server (%s)" % e); return
        self.me = user
        self.net.send("%s %s %s" % (verb, user, pw))
        self.root.after(60, self.pump)

    # ── main UI ──────────────────────────────────────────────────────
    def _build_main(self):
        self.login.destroy()
        self.root.unbind("<Return>")

        outer = tk.Frame(self.root, bg=BG)
        outer.pack(fill="both", expand=True)

        side = tk.Frame(outer, bg=PANEL, width=210)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)

        tk.Label(side, text="  BOOT Chat", bg=PANEL, fg=ACCENT, anchor="w",
                 font=("Segoe UI", 12, "bold")).pack(fill="x", pady=(10, 2))
        self.l_me = tk.Label(side, text="  " + self.me, bg=PANEL, fg=MUTED,
                             anchor="w", font=("Segoe UI", 9))
        self.l_me.pack(fill="x", pady=(0, 8))

        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("S.Treeview", background=PANEL, fieldbackground=PANEL,
                        foreground=FG, borderwidth=0, rowheight=22)
        style.map("S.Treeview", background=[("selected", ACCENT)],
                  foreground=[("selected", "#06131f")])

        self.tree = ttk.Treeview(side, style="S.Treeview", show="tree",
                                 selectmode="browse")
        self.tree.pack(fill="both", expand=True, padx=6)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)

        bar = tk.Frame(side, bg=PANEL)
        bar.pack(fill="x", pady=6)
        tk.Button(bar, text="+ Server", command=self.new_server, bg=BG, fg=FG,
                  relief="flat", font=("Segoe UI", 9)).pack(side="left", padx=(8, 3))
        tk.Button(bar, text="+ Channel", command=self.new_channel, bg=BG, fg=FG,
                  relief="flat", font=("Segoe UI", 9)).pack(side="left", padx=3)
        tk.Button(bar, text="+ DM", command=self.new_dm, bg=BG, fg=FG,
                  relief="flat", font=("Segoe UI", 9)).pack(side="left", padx=3)

        right = tk.Frame(outer, bg=BG)
        right.pack(side="left", fill="both", expand=True)

        self.l_title = tk.Label(right, text="select a channel", bg=BG, fg=FG,
                                anchor="w", font=("Segoe UI", 12, "bold"))
        self.l_title.pack(fill="x", padx=14, pady=(12, 6))

        self.log = tk.Text(right, bg=BG, fg=FG, relief="flat", wrap="word",
                           font=("Consolas", 10), state="disabled",
                           padx=12, pady=6)
        self.log.pack(fill="both", expand=True, padx=6)
        self.log.tag_config("sys",  foreground=MUTED)
        self.log.tag_config("err",  foreground=ERRCOL)
        self.log.tag_config("own",  foreground=OWN)
        self.log.tag_config("who",  foreground=ACCENT)
        self.log.tag_config("time", foreground=MUTED)

        row = tk.Frame(right, bg=BG)
        row.pack(fill="x", padx=12, pady=10)
        self.entry = tk.Entry(row, bg=PANEL, fg=FG, insertbackground=FG,
                              relief="flat", font=("Segoe UI", 11))
        self.entry.pack(side="left", fill="x", expand=True, ipady=7)
        self.entry.bind("<Return>", self.on_send)
        tk.Button(row, text="Send", command=self.on_send, bg=ACCENT, fg="#06131f",
                  relief="flat", width=9, font=("Segoe UI", 10, "bold")).pack(side="left", padx=(8, 0))
        self.entry.focus_set()

    # ── sidebar ──────────────────────────────────────────────────────
    def refresh_tree(self):
        sel = self.tree.selection()
        keep = sel[0] if sel else None
        self.tree.delete(*self.tree.get_children())
        for s in self.servers:
            node = self.tree.insert("", "end", iid="s:" + s, text=s, open=True)
            for ch in self.channels.get(s, []):
                self.tree.insert(node, "end", iid="c:%s:%s" % (s, ch), text="  #" + ch)
        if self.dms:
            d = self.tree.insert("", "end", iid="dmroot", text="Direct messages", open=True)
            for u in self.dms:
                self.tree.insert(d, "end", iid="d:" + u, text="  @" + u)
        if keep and self.tree.exists(keep):
            self.tree.selection_set(keep)

    def on_select(self, _e):
        sel = self.tree.selection()
        if not sel:
            return
        iid = sel[0]
        if iid.startswith("c:"):
            _, srv, ch = iid.split(":", 2)
            self.clear_log()
            self.view = ("chan", srv, ch)
            self.l_title.config(text="#%s  ·  %s" % (ch, srv))
            self.net.send("JOIN %s %s" % (srv, ch))
        elif iid.startswith("d:"):
            user = iid[2:]
            self.clear_log()
            self.view = ("dm", user)
            self.l_title.config(text="@" + user)
            self.net.send("DMHIST " + user)
        elif iid.startswith("s:"):
            self.net.send("CHANNELS " + iid[2:])

    # ── actions ──────────────────────────────────────────────────────
    def _ask(self, title, prompt):
        win = tk.Toplevel(self.root); win.title(title); win.configure(bg=BG)
        win.transient(self.root); win.grab_set(); win.resizable(False, False)
        tk.Label(win, text=prompt, bg=BG, fg=FG, font=("Segoe UI", 10)).pack(padx=18, pady=(16, 6))
        e = tk.Entry(win, bg=PANEL, fg=FG, insertbackground=FG, relief="flat",
                     width=28, font=("Segoe UI", 11))
        e.pack(padx=18, ipady=5); e.focus_set()
        out = {}
        def done(_=None):
            out["v"] = e.get().strip(); win.destroy()
        tk.Button(win, text="OK", command=done, bg=ACCENT, fg="#06131f",
                  relief="flat", width=10).pack(pady=12)
        e.bind("<Return>", done)
        self.root.wait_window(win)
        return out.get("v")

    def new_server(self):
        name = self._ask("New server", "Server name (or an existing one to join):")
        if not name:
            return
        # CREATE first; if it exists the server replies ERR and we join instead
        self.pending_join = name
        self.net.send("CREATE " + name)

    def new_channel(self):
        if not self.view or self.view[0] != "chan":
            messagebox.showinfo("BOOT Chat", "Pick a server's channel first."); return
        name = self._ask("New channel", "Channel name:")
        if name:
            self.net.send("MKCHAN %s %s" % (self.view[1], name))

    def new_dm(self):
        user = self._ask("Direct message", "Username:")
        if not user:
            return
        if user not in self.dms:
            self.dms.append(user); self.dms.sort(); self.refresh_tree()
        if self.tree.exists("d:" + user):
            self.tree.selection_set("d:" + user)

    def on_send(self, _e=None):
        text = self.entry.get().strip()
        if not text or not self.view:
            return
        self.entry.delete(0, "end")
        if self.view[0] == "chan":
            self.net.send("SAY " + text)
        else:
            self.net.send("DM %s %s" % (self.view[1], text))

    # ── log ──────────────────────────────────────────────────────────
    def clear_log(self):
        self.log.config(state="normal"); self.log.delete("1.0", "end")
        self.log.config(state="disabled")

    def add(self, text, tag=None, ts=None):
        self.log.config(state="normal")
        if ts:
            self.log.insert("end", time.strftime("%H:%M ", time.localtime(ts)), "time")
        self.log.insert("end", text + "\n", tag or ())
        self.log.see("end")
        self.log.config(state="disabled")

    # ── incoming ─────────────────────────────────────────────────────
    def pump(self):
        while True:
            try:
                line = self.net.q.get_nowait()
            except queue.Empty:
                break
            self.handle(line)
        self.root.after(80, self.pump)

    def handle(self, line):
        if line == "__DISCONNECTED__":
            if hasattr(self, "log"):
                self.add("-- disconnected from server", "err")
            else:
                self.l_err.config(text="disconnected")
            return

        verb, _, rest = line.partition(" ")

        if verb == "TOKEN":
            return
        if verb == "OK":
            what, _, detail = rest.partition(" ")
            if what == "LOGIN":
                self._build_main()
            elif what == "CREATE":
                self.pending_join = None
                self.net.send("CHANNELS " + detail)
            elif what == "JOINSRV":
                self.pending_join = None
                self.net.send("CHANNELS " + detail)
            return
        if verb == "ERR":
            if self.pending_join and ("exists" in rest):
                # the name was taken, which here means "join it instead"
                name, self.pending_join = self.pending_join, None
                self.net.send("JOINSRV " + name)
                return
            if hasattr(self, "log"):
                self.add("!! " + rest, "err")
            else:
                self.l_err.config(text=rest)
            return
        if verb == "SERVERLIST":
            self.servers = rest.split()
            for s in self.servers:
                self.net.send("CHANNELS " + s)
            self.net.send("DMLIST")
            self.refresh_tree()
            return
        if verb == "CHANLIST":
            parts = rest.split()
            if parts:
                self.channels[parts[0]] = parts[1:]
                self.refresh_tree()
            return
        if verb == "DMLIST":
            self.dms = rest.split()
            self.refresh_tree()
            return
        if verb == "MSG":
            p = rest.split(" ", 4)
            if len(p) == 5:
                srv, ch, who, ts, text = p
                if self.view and self.view[0] == "chan" and self.view[1] == srv and self.view[2] == ch:
                    self.add("%s: %s" % (who, text),
                             "own" if who == self.me else None, int(ts))
            return
        if verb == "DMMSG":
            p = rest.split(" ", 3)
            if len(p) == 4:
                frm, to, ts, text = p
                other = to if frm == self.me else frm
                if other not in self.dms:
                    self.dms.append(other); self.dms.sort(); self.refresh_tree()
                if self.view and self.view[0] == "dm" and self.view[1] == other:
                    self.add("%s: %s" % (frm, text),
                             "own" if frm == self.me else None, int(ts))
            return
        if verb == "JOINED":
            p = rest.split()
            if p:
                self.add("-- %s joined" % p[0], "sys")
            return
        if verb == "LEFT":
            self.add("-- %s left" % rest, "sys")
            return
        if verb == "ONLINE":
            self.add("-- online: " + rest, "who")
            return
        if verb == "HISTEND":
            return

    def on_close(self):
        self.net.close()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description="BOOT Chat v2 desktop client")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6700)
    a = ap.parse_args()
    root = tk.Tk()
    App(root, a.host, a.port)
    root.mainloop()


if __name__ == "__main__":
    main()
