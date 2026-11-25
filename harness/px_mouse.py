#!/usr/bin/env python3
"""px_mouse.py -- the pointer, placed to a PIXEL (v0.9.9).

window_apps.place() puts the pointer on a CELL. Resizing a window needs
better: its grab band is a few pixels either side of the outline. This
finds the arrow's tip by its shape (arrowfind.py) and moves the rest of
the way one count at a time -- drivers/mouse.c moves a slow count one
pixel below 1920 wide, two from 1920 -- until the tip is there, then
drags with the button held, in slow steps, so no movement is
accelerated.
"""
import time
from PIL import Image
from mouse_apps import ev
import arrowfind


def rel(q, axis, v):
    ev(q, [{"type": "rel", "data": {"axis": axis, "value": v}}])
    time.sleep(0.004 if abs(v) > 4 else 0.025)


def walk(q, axis, n, step=4):
    n = int(round(n)); sgn = 1 if n >= 0 else -1; n = abs(n)
    for _ in range(n // step):
        rel(q, axis, step * sgn)
    for _ in range(n % step):
        rel(q, axis, sgn)


def shot(q, path="/tmp/px_mouse.ppm"):
    q.shot(path); time.sleep(0.3)
    return Image.open(path).convert("RGB")


def tip(q):
    import numpy as np
    return arrowfind._by_shape(np.asarray(shot(q), dtype=np.int16))


def park(q):
    for _ in range(50):
        rel(q, "x", -40)
    for _ in range(40):
        rel(q, "y", -40)
    time.sleep(0.3)


def to(q, x, y, per=1.0, tries=6):
    """The arrow's tip at pixel (x, y). per: pixels a slow count moves.
    Returns where it ended up (None if the arrow could not be seen)."""
    at = tip(q)
    if at is None:
        park(q); at = tip(q)
    for _ in range(tries):
        if at is None:
            return None
        dx, dy = x - at[0], y - at[1]
        if dx == 0 and dy == 0:
            return at
        walk(q, "x", dx / per); walk(q, "y", dy / per)
        time.sleep(0.35)
        at = tip(q)
    return at


def drag(q, dx, dy, per=1.0, step=4):
    """Button down where the pointer is, move by (dx, dy) pixels, up."""
    ev(q, [{"type": "btn", "data": {"down": True, "button": "left"}}]); time.sleep(0.25)
    n = int(max(abs(dx), abs(dy)) / per / step) or 1
    sx, sy = dx / per / n, dy / per / n
    fx = fy = 0.0
    for _ in range(n):
        fx += sx; fy += sy
        ix, iy = int(round(fx)), int(round(fy))
        if ix: rel(q, "x", ix); fx -= ix
        if iy: rel(q, "y", iy); fy -= iy
        time.sleep(0.02)
    time.sleep(0.6)
    ev(q, [{"type": "btn", "data": {"down": False, "button": "left"}}]); time.sleep(1.5)


def nudge(q, dx, dy, per=1.0):
    """Move without looking (the shape may not be the arrow any more)."""
    walk(q, "x", dx / per, 1); walk(q, "y", dy / per, 1); time.sleep(0.4)


def click(q, settle=1.0):
    ev(q, [{"type": "btn", "data": {"down": True, "button": "left"}}]); time.sleep(0.08)
    ev(q, [{"type": "btn", "data": {"down": False, "button": "left"}}]); time.sleep(settle)
