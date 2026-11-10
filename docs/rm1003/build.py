#!/usr/bin/env python3
"""Build THS-RM-1003 as a PDF.

The content lives in content.py as plain structures; this file only turns it
into the HTML the stylesheets expect, numbers the sections, and hands the
result to WeasyPrint.

It loads TWO stylesheets, in order:

    manual/style.css      the house style -- A5, cream stock, the typefaces,
                          the spot colour, the callouts, the tables
    docs/rm1003/style.css only what a report needs and a manual does not

That split is deliberate. The look of a HavenDOS document is defined in one
place, and a second document that copied it would drift from the manual the
first time either changed -- which is the same failure as a help list typed
beside the dispatcher, or a test that hand-copies the function it checks.

Needs: weasyprint, and the four house typefaces visible to fontconfig
(Courier Prime, IBM Plex Mono, VT323, Press Start 2P -- the @fontsource npm
packages, converted to TTF). It fails with the reason rather than silently
substituting a default face, because a substituted face is a different
document.

    python3 docs/rm1003/build.py
"""
import html
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOUSE = os.path.join(ROOT, "manual", "style.css")
sys.path.insert(0, HERE)

from content import SECTIONS, META          # noqa: E402

REQUIRED_FACES = ["Courier Prime", "IBM Plex Mono", "VT323", "Press Start 2P"]


# ── cross-references ────────────────────────────────────────────────
# A section reference is written [[Exact Section Title]] and resolved here
# to its number.  A typed "section 18" is a list that describes the
# document, maintained by hand beside the thing it describes, and it drifts
# the first time a section is inserted -- which it already had, in the
# first draft of this file: four of eight references pointed at the wrong
# section.  Same failure as the help list beside the dispatcher, and the
# same fix: generate it, and fail the build when it cannot be resolved.
#
#   [[Title]]   ->  "section 12"
#   [[^Title]]  ->  "Section 12"   (sentence-initial)
import re  # noqa: E402

REF = re.compile(r"\[\[(\^?)(.+?)\]\]")


def resolve_refs(text, index):
    def one(m):
        cap, title = m.group(1), m.group(2)
        if title not in index:
            sys.stderr.write(
                "build: [[%s]] does not name a section.\n"
                "  Sections are:\n%s\n"
                % (title, "\n".join("    %2d  %s" % (n, t)
                                    for t, n in sorted(index.items(),
                                                       key=lambda kv: kv[1]))))
            sys.exit(1)
        return ("Section %d" if cap else "section %d") % index[title]
    return REF.sub(one, text)


def check_fonts():
    """A missing face does not fail the render, it silently changes it."""
    try:
        import subprocess
        have = subprocess.run(["fc-list"], capture_output=True, text=True).stdout
    except Exception:
        sys.stderr.write("  (cannot run fc-list; not checking typefaces)\n")
        return
    missing = [f for f in REQUIRED_FACES if f.lower() not in have.lower()]
    if missing:
        sys.stderr.write(
            "build: these typefaces are not installed: %s\n"
            "  The document would render in a substitute face, which is a\n"
            "  different document. Install the @fontsource packages and\n"
            "  convert them to TTF into ~/.local/share/fonts, then fc-cache.\n"
            % ", ".join(missing))
        sys.exit(1)


# ── blocks ──────────────────────────────────────────────────────────
def render_block(b):
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
    if kind == "box":
        return ('<div class="box"><span class="lab">%s</span><p>%s</p></div>'
                % (b[1], b[2]))
    if kind == "spec":
        rows = "".join('<div><span class="k">%s</span>%s</div>' % (k, v)
                       for k, v in b[1])
        return '<div class="spec">%s</div>' % rows
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
        return ("<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>"
                % (th, tr))
    if kind == "raw":
        return b[1]
    raise ValueError("unknown block %r" % (kind,))


def cover_bars():
    out = []
    for lab, on, total, note in META["cover_bars"]:
        bar = ('<span class="on">%s</span><span class="off">%s</span>'
               % ("#" * on, "." * (total - on)))
        out.append('<div class="row"><span class="lab">%s</span>'
                   '<span class="bar">%s</span>'
                   '<span class="cap">%s</span></div>' % (lab, bar, note))
    return "".join(out)


def build():
    body = []

    body.append('''
<div class="cover">
  <div class="frame">
    <div class="vendor">TECHHAVEN STUDIOS</div>
    <div class="spot"></div>
    <h1>%(title)s</h1>
    <div class="sub">%(subtitle)s</div>
    <div class="bars">%(bars)s</div>
    <div class="meta">
      <div><span class="k">DOCUMENT</span>%(part)s &middot; %(date)s</div>
      <div><span class="k">AUTHOR</span>%(author)s</div>
      <div><span class="k">BASELINE</span>%(baseline)s</div>
      <div><span class="k">SUPERSEDES</span>%(supersedes)s</div>
      <div><span class="k">STATUS</span>%(status)s</div>
    </div>
  </div>
</div>''' % dict(META, bars=cover_bars()))

    toc = "".join('<li><span class="n">%d</span><a href="#s%d">%s</a></li>'
                  % (i, i, s["title"]) for i, s in enumerate(SECTIONS, 1))
    body.append('<div class="toc"><h2><span class="num">&#9632;</span>'
                'Contents</h2><ol>%s</ol></div>' % toc)

    for i, sec in enumerate(SECTIONS, 1):
        body.append('<h2 id="s%d"><span class="num">%d</span>%s</h2>'
                    % (i, i, sec["title"]))
        for b in sec["body"]:
            body.append(render_block(b))

    index = {s["title"]: i for i, s in enumerate(SECTIONS, 1)}
    doc = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
           '<title>HavenDOS &mdash; road to 1.0 (THS-RM-1003)</title></head>'
           '<body>%s</body></html>' % "".join(body))
    return resolve_refs(doc, index)


if __name__ == "__main__":
    check_fonts()
    if not os.path.exists(HOUSE):
        sys.exit("build: cannot find the house stylesheet at %s" % HOUSE)
    from weasyprint import HTML, CSS
    out = os.path.join(ROOT, "docs", "roadmap", "HavenDOS-Road-to-1.0-RM1003.pdf")
    doc = build()
    open(os.path.join(HERE, "rm1003.html"), "w").write(doc)
    HTML(string=doc, base_url=HERE).write_pdf(
        out, stylesheets=[CSS(HOUSE), CSS(os.path.join(HERE, "style.css"))])
    print("wrote %s  %.1f KB  (%d sections)"
          % (out, os.path.getsize(out) / 1024.0, len(SECTIONS)))
