#!/usr/bin/env python3
"""Is the gap in the filler lines the rate limiter, or real message loss?

Sends a run of messages BELOW the rate limit and checks every one arrives in
the reader's DOM, then repeats ABOVE the limit and checks the sender is told.
"""
import os, subprocess, sys, tempfile, time
from playwright.sync_api import sync_playwright

TOOLS = "/home/claude/work/tree/havendos-v0.7.17-source/tools"
CP, WP = 6805, 8105
N = 24
fails = total = 0


def ck(w, c, d=""):
    global fails, total
    total += 1
    print(("  ok    " if c else "  FAIL  ") + w + ("" if c else "  " + str(d)))
    if not c:
        fails += 1


tmp = tempfile.mkdtemp(); db = os.path.join(tmp, "d.db")
chat = subprocess.Popen([sys.executable, os.path.join(TOOLS, "bootchat2_server.py"),
                         "--port", str(CP), "--db", db],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
web = subprocess.Popen([sys.executable, os.path.join(TOOLS, "bootchat2_web.py"),
                        "--port", str(WP), "--chat-port", str(CP)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.8)
url = "http://127.0.0.1:%d/" % WP


def signin(br, name):
    pg = br.new_context(viewport={"width": 1280, "height": 900}).new_page()
    pg.goto(url, wait_until="domcontentloaded"); pg.wait_for_timeout(600)
    pg.fill("#user", name); pg.fill("#pass", "hunter22"); pg.click("#btnReg")
    pg.wait_for_selector("#app:not(.hidden)", timeout=8000)
    return pg


try:
    with sync_playwright() as pw:
        br = pw.chromium.launch(executable_path="/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
        a = signin(br, "ann")
        a.once("dialog", lambda d: d.accept("techhaven"))
        a.locator(".grp", has_text="Servers").locator("button").click()
        a.wait_for_timeout(1200)
        a.locator(".item", has_text="general").first.click()
        a.wait_for_timeout(1000)

        b = signin(br, "bea")
        b.once("dialog", lambda d: d.accept("techhaven"))
        b.locator(".grp", has_text="Servers").locator("button").click()
        b.wait_for_timeout(1200)
        b.locator(".item", has_text="general").first.click()
        b.wait_for_timeout(1000)

        print("\nbelow the rate limit (300ms apart, limit is 4/s)")
        for i in range(N):
            a.fill("#msg", "slow-%02d" % i)
            a.press("#msg", "Enter")
            a.wait_for_timeout(300)
        a.wait_for_timeout(2500)
        b.wait_for_timeout(1500)

        got_b = b.evaluate("() => Array.from(document.querySelectorAll('#log .msg .text'))"
                           ".map(e => e.textContent.trim())")
        want = ["slow-%02d" % i for i in range(N)]
        missing = [w for w in want if not any(w == g or g.startswith(w) for g in got_b)]
        ck("reader received every message sent under the limit",
           not missing, "missing: %s" % missing)
        ck("reader shows them in order",
           [g for g in got_b if g.startswith("slow-")] == want,
           [g for g in got_b if g.startswith("slow-")][:6])

        print("\nabove the rate limit (flood)")
        errs = []
        a.on("console", lambda m: None)
        before = a.evaluate("() => document.querySelectorAll('#log .msg').length")
        for i in range(30):
            a.fill("#msg", "fast-%02d" % i)
            a.press("#msg", "Enter")
            a.wait_for_timeout(60)
        a.wait_for_timeout(2500)
        after = a.evaluate("() => document.querySelectorAll('#log .msg').length")
        toasts = a.evaluate("() => document.querySelectorAll('#toasts .toast').length")
        delivered = after - before
        ck("flood is throttled rather than all delivered", delivered < 30, delivered)
        ck("sender is TOLD when a message is refused (toast shown)",
           toasts > 0, toasts)
        print("        delivered %d of 30, %d toasts visible" % (delivered, toasts))

        # was any text lost silently from the composer?
        ck("a refused message is restored to the composer, not lost",
           a.eval_on_selector("#msg", "e => e.value").startswith("fast-"),
           repr(a.eval_on_selector("#msg", "e => e.value")))
        br.close()
finally:
    for p in (web, chat):
        try: p.terminate(); p.wait(timeout=5)
        except Exception: p.kill()

print("\n%d/%d passed" % (total - fails, total))
sys.exit(1 if fails else 0)
