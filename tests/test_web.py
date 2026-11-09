#!/usr/bin/env python3
"""Drive the real BootChat page in Chromium against a real server.

Nothing here is mocked: bootchat2_server.py and bootchat2_web.py both run,
and the browser does what a person would do.
"""
import os
import subprocess
import sys
import tempfile
import time
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tree", "havendos-v0.7.17-source", "tools")
CHAT_PORT, WEB_PORT = 6801, 8101
SHOTS = os.path.join(HERE, "..", "shots")
os.makedirs(SHOTS, exist_ok=True)

fails, total = 0, 0


def ck(what, cond, detail=""):
    global fails, total
    total += 1
    print(("  ok    " if cond else "  FAIL  ") + what + ("" if cond else "  " + str(detail)))
    if not cond:
        fails += 1


def main():
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "web.db")
    env = dict(os.environ, BOOTCHAT_PUBLIC_NAME="boot.chat")
    chat = subprocess.Popen(
        [sys.executable, os.path.join(TOOLS, "bootchat2_server.py"),
         "--port", str(CHAT_PORT), "--db", db],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    web = subprocess.Popen(
        [sys.executable, os.path.join(TOOLS, "bootchat2_web.py"),
         "--port", str(WEB_PORT), "--chat-port", str(CHAT_PORT),
         "--chat-host", "127.0.0.1"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    time.sleep(1.8)
    for p, n in ((chat, "chat"), (web, "web")):
        if p.poll() is not None:
            print("%s server died:\n%s" % (n, p.stdout.read()))
            return 1

    url = "http://127.0.0.1:%d/" % WEB_PORT
    try:
        with sync_playwright() as pw:
            br = pw.chromium.launch(executable_path="/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
            ctx = br.new_context(viewport={"width": 1280, "height": 860})
            pg = ctx.new_page()
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)

            print("\nload + sign-up")
            pg.goto(url, wait_until="domcontentloaded"); pg.wait_for_timeout(600)
            ck("page renders the wordmark", pg.locator(".auth-head .mark").is_visible())
            ck("sign-in screen shows the configured public name, not an IP:port",
               "boot.chat" in pg.locator("#authhost").inner_text(),
               pg.locator("#authhost").inner_text())
            html = pg.content()
            ck("no chat-server address anywhere in the delivered page",
               str(CHAT_PORT) not in html)

            pg.fill("#user", "kyan")
            pg.fill("#pass", "hunter22")
            pg.click("#btnReg")
            pg.wait_for_selector("#app:not(.hidden)", timeout=8000)
            ck("account created and app shown", pg.locator("#app").is_visible())
            pg.wait_for_timeout(700)
            ck("connection badge reads connected",
               "connected" in pg.locator("#conn").inner_text(),
               pg.locator("#conn").inner_text())

            print("\nserver + channel")
            pg.once("dialog", lambda d: d.accept("techhaven"))
            pg.locator(".grp", has_text="Servers").locator("button").click()
            pg.wait_for_timeout(900)
            pg.locator(".item", has_text="general").first.click()
            pg.wait_for_timeout(900)
            ck("channel opens and header shows it",
               pg.locator("#ctxName").inner_text() == "#general",
               pg.locator("#ctxName").inner_text())

            print("\nsending + grouping")
            for t in ["first message", "second, same author", "third, same author"]:
                pg.fill("#msg", t)
                pg.press("#msg", "Enter")
                pg.wait_for_timeout(320)
            pg.wait_for_timeout(600)
            rows = pg.locator(".msg")
            ck("three messages rendered", rows.count() == 3, rows.count())
            starts = pg.locator(".msg.start")
            ck("consecutive messages group under one header (1 start, not 3)",
               starts.count() == 1, starts.count())
            ck("date separator present",
               pg.locator(".sep").count() == 1 and
               "today" in pg.locator(".sep").first.inner_text().lower(),
               pg.locator(".sep").first.inner_text() if pg.locator(".sep").count() else None)

            print("\nShift+Enter is a newline, not a send")
            pg.click("#msg")
            pg.type("#msg", "line one")
            pg.press("#msg", "Shift+Enter")
            pg.type("#msg", "line two")
            pg.wait_for_timeout(200)
            val = pg.eval_on_selector("#msg", "e => e.value")
            ck("Shift+Enter inserted a newline", "\n" in val, repr(val))
            ck("composer grew past one row",
               pg.eval_on_selector("#msg", "e => e.clientHeight") > 30)
            ck("message was not sent", pg.locator(".msg").count() == 3)
            pg.press("#msg", "Enter")
            pg.wait_for_timeout(500)
            ck("Enter sent the multi-line message", pg.locator(".msg").count() == 4)

            print("\nXSS safety")
            pg.fill("#msg", "<img src=x onerror=alert(1)><b>not bold</b>")
            pg.press("#msg", "Enter")
            pg.wait_for_timeout(600)
            ck("markup is rendered as literal text, not parsed",
               pg.locator(".msg .text b").count() == 0 and
               pg.locator(".msg .text img").count() == 0)
            ck("the raw text is visible to the reader",
               "<b>not bold</b>" in pg.locator(".msg .text").last.inner_text())

            print("\nedit + delete (real verbs, real persistence)")
            last = pg.locator(".msg").last
            last.hover()
            last.locator('button[aria-label="Edit"]').click()
            pg.wait_for_timeout(300)
            pg.fill(".edit-box", "edited content")
            pg.press(".edit-box", "Enter")
            pg.wait_for_timeout(700)
            ck("edit applied and marked",
               "edited content" in pg.locator(".msg .text").last.inner_text() and
               "(edited)" in pg.locator(".msg .text").last.inner_text(),
               pg.locator(".msg .text").last.inner_text())

            n_before = pg.locator(".msg").count()
            pg.once("dialog", lambda d: d.accept())
            last = pg.locator(".msg").last
            last.hover()
            last.locator('button[aria-label="Delete"]').click()
            pg.wait_for_timeout(800)
            ck("delete removed the message", pg.locator(".msg").count() == n_before - 1,
               pg.locator(".msg").count())

            print("\nmentions from the real user table")
            ctx2 = br.new_context(viewport={"width":1280,"height":860})
            pg2 = ctx2.new_page()
            pg2.goto(url, wait_until="domcontentloaded"); pg2.wait_for_timeout(600)
            pg2.fill("#user", "sanne")
            pg2.fill("#pass", "hunter22")
            pg2.click("#btnReg")
            pg2.wait_for_selector("#app:not(.hidden)", timeout=8000)
            pg2.once("dialog", lambda d: d.accept("techhaven"))
            pg2.locator(".grp", has_text="Servers").locator("button").click()
            pg2.wait_for_timeout(1000)
            pg2.locator(".item", has_text="general").first.click()
            pg2.wait_for_timeout(900)

            pg.reload(wait_until="domcontentloaded"); pg.wait_for_timeout(600)
            pg.wait_for_timeout(1200)
            pg.locator(".item", has_text="general").first.click()
            pg.wait_for_timeout(1000)
            pg.click("#msg")
            pg.type("#msg", "hey @sa")
            pg.wait_for_timeout(450)
            ck("mention popup appears", pg.locator("#ment:not(.hidden)").is_visible())
            ck("suggests the real account", "sanne" in pg.locator("#ment").inner_text(),
               pg.locator("#ment").inner_text())
            pg.press("#msg", "Enter")
            pg.wait_for_timeout(250)
            v = pg.eval_on_selector("#msg", "e => e.value")
            ck("keyboard selection inserts the name", v.strip() == "hey @sanne", repr(v))
            pg.press("#msg", "Enter")
            pg.wait_for_timeout(900)
            ck("mention is highlighted in the message",
               pg.locator(".msg .men").count() >= 1)

            print("\nmention reaches the other user, highlighted as theirs")
            pg2.wait_for_timeout(1200)
            ck("recipient sees the mention styled as self-mention",
               pg2.locator(".msg .men.me").count() >= 1,
               pg2.locator(".msg .men").count())
            ck("recipient's row is flagged", pg2.locator(".msg.hasme").count() >= 1)

            print("\nnew-message indicator (no force-scroll)")
            for i in range(28):
                pg2.fill("#msg", "filler line %d" % i)
                pg2.press("#msg", "Enter")
                pg2.wait_for_timeout(90)
            pg.wait_for_timeout(1500)
            pg.eval_on_selector("#log", "e => e.scrollTop = 0")
            pg.wait_for_timeout(400)
            top_before = pg.eval_on_selector("#log", "e => e.scrollTop")
            pg2.fill("#msg", "arriving while you are scrolled up")
            pg2.press("#msg", "Enter")
            pg.wait_for_timeout(1400)
            top_after = pg.eval_on_selector("#log", "e => e.scrollTop")
            ck("reader was NOT yanked to the bottom", abs(top_after - top_before) < 40,
               (top_before, top_after))
            ck("new-message pill is showing", pg.locator("#jump.show").is_visible())
            ck("pill counts the new messages",
               "new message" in pg.locator("#jumpTxt").inner_text(),
               pg.locator("#jumpTxt").inner_text())
            pg.click("#jump")
            pg.wait_for_timeout(1100)
            ck("clicking it returns to the newest message",
               not pg.locator("#jump").is_visible() or
               "show" not in (pg.get_attribute("#jump", "class") or ""))

            print("\nreply")
            pg.locator(".msg").nth(2).hover()
            pg.locator(".msg").nth(2).locator('button[aria-label="Reply"]').click()
            pg.wait_for_timeout(300)
            ck("reply bar appears", pg.locator("#replybar:not(.hidden)").is_visible())
            pg.fill("#msg", "answering that")
            pg.press("#msg", "Enter")
            pg.wait_for_timeout(900)
            ck("reply renders a quote of the original",
               pg.locator(".msg .quote").count() >= 1)

            pg.screenshot(path=os.path.join(SHOTS, "desktop.png"), full_page=False)

            print("\nsession persistence across reload")
            pg.reload(wait_until="domcontentloaded"); pg.wait_for_timeout(600)
            pg.wait_for_timeout(1800)
            ck("still signed in after refresh (token replay)",
               pg.locator("#app").is_visible() and
               pg.locator("#auth").get_attribute("class").find("hidden") >= 0)

            print("\nreconnect after the server drops")
            chat.terminate(); chat.wait(timeout=5)
            pg.wait_for_timeout(2500)
            cls = pg.get_attribute("#conn", "class") or ""
            ck("connection badge leaves the connected state",
               "on" not in cls.split(), cls)
            ck("a human-readable notice is shown, not a stack trace",
               pg.locator(".errline").count() >= 1 or "offline" in
               pg.locator("#conn").inner_text() or "reconnect" in
               pg.locator("#conn").inner_text().lower(),
               pg.locator("#conn").inner_text())

            print("\nmobile layout")
            m = br.new_context(viewport={"width":390,"height":844}).new_page()
            m.goto(url, wait_until="domcontentloaded"); m.wait_for_timeout(600)
            m.wait_for_timeout(1500)
            if m.locator("#auth").is_visible():
                pass
            ow = m.evaluate("document.documentElement.scrollWidth")
            cw = m.evaluate("document.documentElement.clientWidth")
            ck("no horizontal overflow at 390px", ow <= cw + 1, (ow, cw))
            m.screenshot(path=os.path.join(SHOTS, "mobile.png"))

            ck("no uncaught JS errors during the whole run",
               len([e for e in errors if "favicon" not in e.lower()]) == 0,
               errors[:3])
            br.close()
    finally:
        for p in (web, chat):
            try:
                p.terminate(); p.wait(timeout=5)
            except Exception:
                p.kill()

    print("\n%d/%d passed" % (total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
