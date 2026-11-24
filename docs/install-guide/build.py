#!/usr/bin/env python3
"""Build THS-IG-1001, "Installing HavenDOS beside Windows", as a PDF.

Same pipeline and house style as THS-RM-1005 (docs/rm1005/build.py): the
content is plain structures in content.py; this turns them into HTML and
hands it to WeasyPrint with three stylesheets, in order --

    manual/style.css          the house style
    docs/rm1005/style.css     the bordered report cover
    docs/install-guide/style.css   this guide's running head and key-caps

Figures are screenshots in img/, taken from QEMU by
harness/guide_shots.py so they show the release the guide is for.

    python3 docs/install-guide/build.py
"""
import html
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from content import SECTIONS, META          # noqa: E402

# docs/rm1005/build.py, by path: this file is also called build.py, and that
# one also imports a module called content -- which is already this guide's
# by the time it runs, and it only needs its functions.
import importlib.util                       # noqa: E402
_spec = importlib.util.spec_from_file_location(
    "rm1005_build", os.path.join(ROOT, "docs", "rm1005", "build.py"))
rm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rm)

STYLES = [os.path.join(ROOT, "manual", "style.css"),
          os.path.join(ROOT, "docs", "rm1005", "style.css"),
          os.path.join(HERE, "style.css")]
OUT = os.path.join(ROOT, "docs", "HavenDOS-Install-Guide.pdf")


def block(b):
    if b[0] == "fig":
        _, fn, lab, cap = b
        path = os.path.join(HERE, "img", fn)
        if not os.path.exists(path):
            sys.exit("build: figure %s is missing -- run harness/guide_shots.py" % path)
        return ('<figure><img src="img/%s"/><figcaption><span class="lab">%s'
                '</span> &nbsp;%s</figcaption></figure>' % (fn, lab, cap))
    return rm.render_block(b)


def build():
    need = "".join('<div class="row"><span class="lab">%s</span>'
                   '<span class="val">%s</span></div>' % kv for kv in META["need"])
    body = ['''
<div class="cover"><div class="frame">
  <div class="vendor">TECHHAVEN STUDIOS</div>
  <div class="spot"></div>
  <h1>%(title)s</h1>
  <div class="sub">%(subtitle)s</div>
  <div class="need"><div class="needhead">YOU WILL NEED</div>%(need)s</div>
  <div class="meta">
    <div><span class="k">DOCUMENT</span>%(part)s &middot; %(date)s</div>
    <div><span class="k">FOR</span>HavenDOS %(release)s</div>
  </div>
</div></div>''' % dict(META, need=need)]
    toc = "".join('<li><span class="n">%d</span><a href="#s%d">%s</a></li>'
                  % (i, i, s["title"]) for i, s in enumerate(SECTIONS, 1))
    body.append('<div class="toc"><h2><span class="num">&#9632;</span>'
                'Contents</h2><ol>%s</ol></div>' % toc)
    for i, sec in enumerate(SECTIONS, 1):
        body.append('<h2 id="s%d"><span class="num">%d</span>%s</h2>'
                    % (i, i, sec["title"]))
        body.extend(block(b) for b in sec["body"])
    index = {s["title"]: i for i, s in enumerate(SECTIONS, 1)}
    doc = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
           '<title>Installing HavenDOS beside Windows (THS-IG-1001)</title>'
           '</head><body>%s</body></html>' % "".join(body))
    return rm.resolve_refs(doc, index)


if __name__ == "__main__":
    rm.check_fonts()
    from weasyprint import HTML, CSS
    doc = build()
    HTML(string=doc, base_url=HERE).write_pdf(
        OUT, stylesheets=[CSS(s) for s in STYLES])
    print("wrote %s  %.1f KB  (%d sections)"
          % (OUT, os.path.getsize(OUT) / 1024.0, len(SECTIONS)))
