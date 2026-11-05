"""Builds assets/models/mega32x.glb: Sega's 32X (the Mega Drive's), from photos of a real
one.

    python3 tools/models/mega32x.py

Needs the same as gamegear.py. Units are millimetres here and metres in the file. The
origin is where the 32X meets the mouth of the Mega Drive's slot, under the middle of its
own slot: its plug goes down from there into the Mega Drive, its head stands over it and
the front faces +z.
"""

import os
import sys

import numpy as np
import shapely.geometry as sg
from shapely import affinity

sys.path.insert(0, os.path.dirname(__file__))
from glb import Glb  # noqa: E402
from gamegear import logo_mesh, merge, polygon_mesh  # noqa: E402
from gamegear_front import LOGOS  # noqa: E402
from mega32x_front import BADGE, PANEL  # noqa: E402
from sdf import box, cut, extrude, mesh, round_rect, smax, smin  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "assets", "models")
CREDIT = "plumsg (made by tools/models/mega32x.py)"

# The head: 210 x 106 mm, from 5 mm over the Mega Drive's slot to 62; its middle lies
# 10 mm behind the slot. The plug goes 34 mm down into the Mega Drive.
W, D = 210.0, 106.0
ZC = -10.0
BOTTOM, TOP = 5.0, 62.0
PLUG = -34.0
# The raised track around the slot on top, the slot in it.
TRACK_TOP = TOP + 6.0
SLOT_HALF, SLOT_Z = 60.0, 7.0
# The wave between the glossy top and the matt bottom: high at the ends, low in the
# middle of the front, under the badge.


def wave(x):
    return BOTTOM + 30.0 - 17.0 * np.exp(-(x / 62.0) ** 2)


GLOSS = (0.028, 0.028, 0.032)
MATT = (0.07, 0.07, 0.074)
HOLE = (0.006, 0.006, 0.008)
PANEL_GREY = (0.07, 0.07, 0.075)
WHITE = (0.82, 0.82, 0.8)
FLAP = (0.05, 0.05, 0.056)
CONTACT = (0.55, 0.5, 0.42)


def footprint(x, z, grow=0.0):
    """The head seen from above: rounded at both ends, the front bulging in the middle."""
    d = round_rect(x, z, 0, ZC, W / 2 + grow, D / 2 + grow, 42 + grow)
    bulge = np.sqrt((x / 1.9) ** 2 + (z - (ZC + D / 2 - 30)) ** 2) - 36 - grow
    return smin(d, bulge, 12)


def head(x, y, z):
    f = footprint(x, z)
    # The top rounds over softly all round; the bottom edge less.
    d = extrude(f, y, BOTTOM, TOP, 3.0)
    roof = np.sqrt(np.maximum(f + 16, 0) ** 2 + np.maximum(y - (TOP - 16), 0) ** 2) - 16
    d = smax(d, roof, 2.0)
    # Below the wave the matt half stands out a little further, its edge a step.
    lower = np.maximum(extrude(footprint(x, z, 3.0), y, BOTTOM, TOP, 2.0), y - wave(x))
    d = smin(d, lower, 0.6)
    d = cut(d, np.maximum(np.abs(y - wave(x)) - 0.35, -(footprint(x, z) + 2.2)), 0.2)
    # The track around the slot, its inner step, and the slot.
    track = extrude(round_rect(x, z, 0, SLOT_Z, 78, 27, 26), y, TOP - 6, TRACK_TOP, 2.5)
    d = smin(d, track, 2.0)
    d = cut(d, extrude(round_rect(x, z, 0, SLOT_Z, 70, 19.5, 19), y, TRACK_TOP - 3.0, TRACK_TOP + 5, 1.0), 0.6)
    d = cut(d, box(x, y, z, -SLOT_HALF, SLOT_HALF, PLUG - 5, TRACK_TOP + 5, SLOT_Z - 9, SLOT_Z + 9, 1.5), 0.4)
    # The vents at both ends: four slits each, high up towards the front.
    for side in (-1, 1):
        for i in range(4):
            yy = TOP - 22 + 3.4 * i
            ex = side * W / 2
            d = cut(d, box(x, y, z, ex - 12, ex + 12, yy - 0.75, yy + 0.75, ZC + 8, ZC + 24, 0.6), 0.3)
    # The plug underneath, a block that goes into the Mega Drive's slot; a wider foot
    # under the head where it meets it.
    d = smin(d, box(x, y, z, -50, 50, PLUG, BOTTOM + 2, -9.5, 9.5, 1.5), 0.6)
    d = smin(d, box(x, y, z, -58, 58, 0, BOTTOM + 2, -22, 22, 2.0), 1.0)
    return d


def colour(v):
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    c = np.tile(np.array(GLOSS, np.float32), (len(v), 1))
    c[y < wave(x) + 0.2] = MATT
    hole = (np.abs(x) < SLOT_HALF + 0.3) & (np.abs(z - SLOT_Z) < 9.3) & (y < TRACK_TOP - 3.2)
    hole |= (np.abs(x) > W / 2 - 14) & (y > TOP - 25) & (y < TOP - 10) & (z > ZC + 7) & (z < ZC + 25) & (np.abs(x) < W / 2 - 0.8)
    c[hole] = HOLE
    return c


def front_z(x, y):
    """Where the head's front is, at (x, y): a search along z from the front."""
    lo = np.full(np.shape(x), ZC, float)
    hi = np.full(np.shape(x), ZC + D, float)
    for _ in range(40):
        mid = (lo + hi) / 2
        inside = head(x, y, mid) < 0
        lo = np.where(inside, mid, lo)
        hi = np.where(inside, hi, mid)
    return lo


def on_front(part, lift, cx, cy, tilt):
    """A flat part made facing +z around (0, 0), laid on the front at (cx, cy) and lifted
    `lift` mm off it; tilt leans its top back, as the front does there."""
    v, f, n = part
    v = v.copy()
    t = np.radians(tilt)
    x, y = v[:, 0] + cx, cy + v[:, 1] * np.cos(t)
    z = front_z(x, y) + lift + v[:, 2]
    v = np.stack([x, y, z], axis=1)
    n = n.copy()
    n[:, 1], n[:, 2] = n[:, 1] * np.cos(t) + n[:, 2] * np.sin(t), n[:, 2] * np.cos(t) - n[:, 1] * np.sin(t)
    return v.astype(np.float32), f, n.astype(np.float32)


def console():
    g = Glb()
    to_m = 1 / 1000.0

    def add(part, col, name, shine=0.5, colours=None):
        v, f, n = part
        g.add(v * to_m, f, n, g.material(name, col, roughness=shine), colours=colours)

    print("32x: head")
    v, f, n = mesh(head, (-W / 2 - 3, PLUG, ZC - D / 2 - 3), (W / 2 + 3, TRACK_TOP, ZC + D / 2 + 22), 0.35, keep=0.08)
    # Glossy above the wave, matt below: two materials.
    tri = v[f].mean(axis=1)
    low = tri[:, 1] < wave(tri[:, 0])
    cols = colour(v)
    for sel, name, shine in ((~low, "glossy", 0.12), (low, "matt", 0.6)):
        used, faces = np.unique(f[sel], return_inverse=True)
        g.add(v[used] * to_m, faces.reshape(-1, 3), n[used], g.material(name, (1, 1, 1), roughness=shine), colours=cols[used])
    # The slot's flaps, closed in the middle, a little under its mouth; the plug's contacts.
    for s in (-1, 1):
        flap = polygon_mesh(sg.box(-SLOT_HALF + 1, -8.5, SLOT_HALF - 1, -0.3), 0, 1.2)
        fv, ff, fn = flap
        fv = np.stack([fv[:, 0], TRACK_TOP - 9 + fv[:, 2], SLOT_Z + s * -fv[:, 1]], axis=1).astype(np.float32)
        fn = np.stack([fn[:, 0], fn[:, 2], s * -fn[:, 1]], axis=1).astype(np.float32)
        add((fv, ff if s < 0 else ff[:, ::-1].copy(), fn), FLAP, "flap", 0.5)

    print("32x: badge")
    # The badge: its grey panel, then the white lines and MEGA DRIVE 32X traced from a
    # photo, and SEGA (the clean logo) in the arch at its bottom.
    cy, tilt = TOP - 13.0, 48.0
    panel = sg.Polygon(PANEL).buffer(-0.6)
    add(on_front(polygon_mesh(panel, 0, 0.2), 0.1, 0, cy, tilt), PANEL_GREY, "badge panel", 0.3)
    keep = []
    for o, h in BADGE:
        p = sg.Polygon(o, h)
        c = p.centroid
        if not panel.buffer(1.5).contains(c):
            continue
        if -11 < c.x < 13 and -14 < c.y < -4.5 and p.area < 25:
            continue  # the traced SEGA, blurred: the clean logo instead
        keep.append(p)
    lines = polygon_mesh(sg.MultiPolygon(keep).buffer(0), 0, 0.15)
    sega = logo_mesh(LOGOS["sega"], -8.6, -12.6, 5.2, 0, depth=0.15)
    add(on_front(merge([lines, sega]), 0.3, 0, cy, tilt), WHITE, "badge print", 0.5)

    path = os.path.join(OUT, "mega32x.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    console()
