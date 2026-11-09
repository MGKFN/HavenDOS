#!/usr/bin/env python3
"""
Build the HavenDOS operator's manual as a PDF.

The content lives in content.py as plain structures; this file only turns it
into the HTML the stylesheet expects, numbers the sections and figures, and
hands the result to WeasyPrint.
"""
import os, sys, html, base64, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from content import SECTIONS, META, SHOTS_DIR

fig_counter = {}
toc = []


def img_data(path):
    with open(path, "rb") as f:
        return "data:image/png;base64," + base64.b64encode(f.read()).decode()


def figure(sec_no, spec):
    """spec = (filename, caption)"""
    fn, cap = spec
    p = os.path.join(SHOTS_DIR, fn)
    if not os.path.exists(p):
        cands = [f for f in sorted(os.listdir(SHOTS_DIR))
                 if f.endswith(".png") and f.split("-", 1)[-1] == fn]
        if not cands:
            cands = [f for f in sorted(os.listdir(SHOTS_DIR))
                     if f.endswith(".png") and fn.replace(".png", "") in f]
        if not cands:
            sys.stderr.write("  missing screenshot: %s\n" % fn)
            return ""
        p = os.path.join(SHOTS_DIR, cands[0])
    fig_counter[sec_no] = fig_counter.get(sec_no, 0) + 1
    lab = "FIGURE %d-%d" % (sec_no, fig_counter[sec_no])
    return ('<figure><img src="%s"/><figcaption>'
            '<span class="lab">%s</span>&nbsp;&nbsp;%s</figcaption></figure>'
            % (img_data(p), lab, html.escape(cap)))


def render_block(sec_no, b):
    kind = b[0]
    if kind == "p":
        return "<p>%s</p>" % b[1]
    if kind == "h3":
        return "<h3>%s</h3>" % b[1]
    if kind == "ul":
        return "<ul>%s</ul>" % "".join("<li>%s</li>" % x for x in b[1])
    if kind == "ol":
        return "<ol>%s</ol>" % "".join("<li>%s</li>" % x for x in b[1])
    if kind == "crt":
        return '<pre class="crt">%s</pre>' % html.escape(b[1].strip("\n"))
    if kind == "note":
        return '<div class="note"><span class="lab">NOTE</span>%s</div>' % b[1]
    if kind == "caution":
        return ('<div class="caution"><span class="lab">CAUTION</span>%s</div>'
                % b[1])
    if kind == "fig":
        return figure(sec_no, b[1])
    if kind == "figrow":
        return ('<div class="figrow">%s</div>'
                % "".join(figure(sec_no, s) for s in b[1]))
    if kind == "table":
        head, rows = b[1], b[2]
        th = "".join("<th>%s</th>" % h for h in head)
        tr = ""
        for r in rows:
            cells = "".join(
                '<td class="cmd">%s</td>' % c if i == 0 and len(r) > 1
                else "<td>%s</td>" % c
                for i, c in enumerate(r))
            tr += "<tr>%s</tr>" % cells
        return "<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>" % (th, tr)
    if kind == "raw":
        return b[1]
    raise ValueError("unknown block %r" % kind)


def build():
    body = []
    # ── cover ────────────────────────────────────────────────────────
    body.append('''
<div class="cover">
  <div class="rule-top"></div>
  <div class="inner">
    <div class="vendor">TECHHAVEN<br/>STUDIOS</div>
    <h1>HavenDOS</h1>
    <div class="ver">v%(ver)s %(channel)s<br/>&ldquo;%(codename)s&rdquo;</div>
    <div class="kind">OPERATOR&rsquo;S MANUAL</div>
    <div class="crt">%(crt)s</div>
  </div>
  <div class="foot"><span>%(part)s</span><span>%(date)s</span></div>
</div>''' % META)

    # ── contents ─────────────────────────────────────────────────────
    toc_items = []
    for i, sec in enumerate(SECTIONS, 1):
        toc_items.append('<li><span class="n">%d</span>'
                         '<a href="#s%d">%s</a></li>' % (i, i, sec["title"]))
    body.append('<div class="toc"><h2><span class="num">&#9632;</span>'
                'Contents</h2><ol>%s</ol></div>' % "".join(toc_items))

    # ── sections ─────────────────────────────────────────────────────
    for i, sec in enumerate(SECTIONS, 1):
        body.append('<h2 id="s%d"><span class="num">%d</span>%s</h2>'
                    % (i, i, sec["title"]))
        for b in sec["body"]:
            body.append(render_block(i, b))

    doc = ('<!doctype html><html><head><meta charset="utf-8">'
           '<title>HavenDOS Operator\'s Manual</title></head>'
           '<body>%s</body></html>' % "".join(body))
    return doc


if __name__ == "__main__":
    from weasyprint import HTML
    out = os.path.join(HERE, "HavenDOS-0.8-Beta-Manual.pdf")
    doc = build()
    open(os.path.join(HERE, "manual.html"), "w").write(doc)
    HTML(string=doc, base_url=HERE).write_pdf(
        out, stylesheets=[os.path.join(HERE, "style.css")])
    print("wrote", out, "%.1f KB" % (os.path.getsize(out) / 1024.0))
