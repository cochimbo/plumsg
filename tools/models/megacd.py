"""Builds assets/models/megacd.glb and megacd-tray.glb: Sega's first Mega-CD (the one
with the motorised tray, HAA-2910) and its tray, from photos of the real one.

    python3 tools/models/megacd.py

Needs the same as gamegear.py. Units are millimetres here and metres in the files. The
Mega-CD stands on its feet facing +z, the middle of its footprint at the origin; the
Mega Drive sits on its top against the connector that rises at its right. The tray is
saved closed, where it sits in the Mega-CD, so the shelf only slides it along z.
"""

import os
import sys

import numpy as np
import shapely.geometry as sg
from shapely import affinity

sys.path.insert(0, os.path.dirname(__file__))
from glb import Glb  # noqa: E402
from gamegear import FONT_BOLD, FONT_ITALIC, FONT_REGULAR, logo_mesh, merge, polygon_mesh, small, text_mesh  # noqa: E402
from gamegear_front import LOGOS  # noqa: E402
from megacd_front import LOGOS as PRINTED  # noqa: E402
from sdf import box, circle, cut, extrude, mesh, round_rect, smax, smin  # noqa: E402

# The shapes below are measured on the photos, in their own millimetres; the real one is
# as wide as the Mega Drive that sits on it (280 mm), so the files are scaled to that.
SCALE = 280.0 / 300.0
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "assets", "models")
CREDIT = "plumsg (made by tools/models/megacd.py)"

# The body: 300 x 230 mm and 64 high, on feet 8 mm tall; the seam between its two halves.
W, D = 300.0, 230.0
FEET, TOP = 8.0, 72.0
SEAM = 33.0
FRONT = D / 2
# The glossy panel across the front, a long rounded band, and the tray's door in it.
PANEL = (-135.5, 12.5, 135.5, 60.5)
DOOR = (-116.5, 31.0, 28.5, 52.0)
# The connector that rises at the right, its face (where the Mega Drive's right side
# meets it) and its length along z.
# It covers the Mega Drive's right side, nearly up to its top.
CONN_X, CONN_Z0, CONN_Z1, CONN_TOP = 128.0, -78.0, 22.0, TOP + 52.0
# READY and ACCESS: the windows the lights shine through (x0, y0, x1, y1).
READY = (44.7, 35.7, 67.7, 42.0)
ACCESS = (92.0, 35.7, 115.9, 42.0)

BLACK = (0.035, 0.035, 0.04)
GLOSS = (0.02, 0.02, 0.024)
HOLE = (0.006, 0.006, 0.008)
SILVER = (0.55, 0.56, 0.58)
PRINT = (0.8, 0.8, 0.8)
LINE = (0.42, 0.43, 0.46)
WINDOW = (0.32, 0.34, 0.4)
GOLD = (0.78, 0.6, 0.12)
GREEN = (0.05, 0.55, 0.38)
RED = (0.75, 0.1, 0.06)
PIN = (0.8, 0.62, 0.24)
RCA_RED = (0.6, 0.04, 0.04)
RCA_WHITE = (0.75, 0.75, 0.73)


def stadium(x0, y0, x1, y1):
    r = (y1 - y0) / 2
    return sg.box(x0 + r, y0 + r, x1 - r, y1 - r).buffer(r, quad_segs=24)


def body(x, y, z):
    ax = np.abs(x)
    d = extrude(round_rect(x, z, 0, 0, W / 2, D / 2, 10), y, FEET, TOP, 2.0)
    # The top's edges round over, the front's much more softly.
    top = np.sqrt(np.maximum(y - (TOP - 5), 0) ** 2 + np.maximum(ax - (W / 2 - 5), 0) ** 2) - 5
    back = np.sqrt(np.maximum(y - (TOP - 5), 0) ** 2 + np.maximum(-z - (D / 2 - 5), 0) ** 2) - 5
    front = np.sqrt(np.maximum(y - (TOP - 11), 0) ** 2 + np.maximum(z - (FRONT - 11), 0) ** 2) - 11
    d = smax(smax(smax(d, top, 1.5), back, 1.5), front, 1.5)
    # The lower half is set in a little, the seam a groove around the sides and back.
    lower = extrude(round_rect(x, z, 0, -1, W / 2 - 1.2, D / 2 - 0.2, 9), y, FEET - 2, SEAM, 0.6)
    d = cut(d, smax(-lower, extrude(round_rect(x, z, 0, 0, W / 2 + 5, D / 2 + 5, 10), y, FEET - 2, SEAM - 0.01), 0.1), 0.5)
    d = cut(d, box(x, y, z, -W / 2 - 5, W / 2 + 5, SEAM - 0.3, SEAM + 0.5, -D / 2 - 5, FRONT - 12, 0.2), 0.2)
    # The panel's band sits in a shallow recess; the tray's mouth behind its door.
    pr = (PANEL[3] - PANEL[1]) / 2
    band = round_rect(x, y, 0, (PANEL[1] + PANEL[3]) / 2, (PANEL[2] - PANEL[0]) / 2 + 1.2, pr + 1.2, pr + 1.2)
    d = cut(d, extrude(band, z, FRONT - 1.8, FRONT + 5), 0.6)
    d = cut(d, box(x, y, z, DOOR[0] + 1, DOOR[2] - 1, DOOR[1] + 1, DOOR[3] - 1, FRONT - 40, FRONT + 5, 0.5), 0.3)
    # Under the panel, between the front feet, a strip of slits.
    for i in range(36):
        sx = -70 + 4 * i
        d = cut(d, box(x, y, z, sx - 0.9, sx + 0.9, FEET - 1, FEET + 3.2, FRONT - 3, FRONT + 5, 0.3), 0.2)
    # The feet: wide rounded pads at the front, smaller ones at the back.
    for fx in (-100.0, 96.0):
        d = smin(d, extrude(round_rect(x, z, fx, 88, 28, 18, 10), y, 0, FEET + 1), 0.8)
    for fx in (-100.0, 97.0):
        d = smin(d, extrude(round_rect(x, z, fx, -92, 26, 14, 6), y, 0, FEET + 1), 0.8)
    # On top: the plate the Mega Drive stands on (a shallow step), its slots and holes,
    # the oblong bump at the back and the screw in the middle.
    plate = round_rect(x, z, 22.5, -10, 104, 99, 3)
    d = cut(d, extrude(np.abs(plate) - 0.5, y, TOP - 0.6, TOP + 5), 0.2)
    for x0, x1, z0, z1 in ((-97, -56, -98, -90), (58, 98, -98, -90), (-135, -81, 87, 100), (67, 121, 89, 102)):
        d = cut(d, box(x, y, z, x0, x1, TOP - 4, TOP + 5, z0, z1, 3.5), 0.4)
    for hx, hz in ((-58.5, -71), (33.5, -70), (-74.5, 52), (76.5, 54)):
        d = cut(d, box(x, y, z, hx - 6, hx + 6, TOP - 8, TOP + 5, hz - 10, hz + 10, 0.6), 0.3)
    d = cut(d, extrude(np.abs(round_rect(x, z, 26, -56.5, 14, 7.5, 7.4)) - 0.4, y, TOP - 0.5, TOP + 5), 0.2)
    d = cut(d, extrude(circle(x, z, 6.5, -14, 1.6), y, TOP - 3, TOP + 5), 0.2)
    # The vents along the back's top edge, right of the middle.
    for i in range(16):
        vx = 44 + 3.4 * i
        d = cut(d, box(x, y, z, vx - 0.8, vx + 0.8, TOP - 6, TOP + 5, -D / 2 - 5, -D / 2 + 5, 0.3), 0.2)
    # On the back: the grille, right of the middle, its ribs standing out.
    d = cut(d, box(x, y, z, 36, 114, 46, TOP - 3, -D / 2 - 5, -D / 2 + 2.5, 1.0), 0.4)
    for i in range(22):
        rx = 38.5 + 3.47 * i
        d = smin(d, box(x, y, z, rx - 1.1, rx + 1.1, 46.5, TOP - 3.5, -D / 2 + 1, -D / 2 + 3.4, 1.0), 0.3)
    # The sockets' panel on the lower half at the left: line out (R, L), mixing in, DC in.
    d = cut(d, box(x, y, z, -110, -42, 10, 31, -D / 2 - 5, -D / 2 + 3, 0.8), 0.3)
    for sx in (-50.5, -66.5):
        d = smin(d, box(x, y, z, sx - 5, sx + 5, 16, 27, -D / 2 + 1.5, -D / 2 + 3.5, 1.0), 0.3)
    d = cut(d, extrude(circle(x, y, -81.5, 21.5, 2.2), z, -D / 2 - 5, -D / 2 + 8), 0.2)
    d = cut(d, extrude(circle(x, y, -96.0, 21.5, 2.6), z, -D / 2 - 5, -D / 2 + 8), 0.2)
    # The lock's channel at the front of the left side.
    d = cut(d, box(x, y, z, -W / 2 - 5, -W / 2 + 2.5, 22, 62, 84, 104, 1.0), 0.3)
    return d


def body_colour(v):
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    c = np.tile(np.array(BLACK, np.float32), (len(v), 1))
    hole = (x > DOOR[0]) & (x < DOOR[2]) & (y > DOOR[1]) & (y < DOOR[3]) & (z < FRONT - 1.2)
    hole |= (np.abs(np.sqrt((x + 81.5) ** 2 + (y - 21.5) ** 2)) < 2.4) & (z < -D / 2 + 6)
    hole |= (np.abs(np.sqrt((x + 96.0) ** 2 + (y - 21.5) ** 2)) < 2.8) & (z < -D / 2 + 6)
    hole |= (y > TOP - 8.5) & (y < TOP - 1.2)  # the holes and slots in the top
    hole |= (z < -D / 2 + 4) & (y > TOP - 6.5) & (y < TOP - 1) & (x > 42) & (x < 100)
    hole |= (z > -D / 2 + 0.5) & (z < -D / 2 + 1.6) & (y > 46) & (y < TOP - 3) & (x > 36) & (x < 114)
    c[hole] = HOLE
    return c


def lock(x, y, z):
    """The lock's lever in its channel."""
    return box(x, y, z, -W / 2 + 0.3, -W / 2 + 3.5, 40, 54, 86, 102, 1.0)


def connector(x, y, z):
    """The cover on the right side that the Mega Drive's right side goes into: a thick
    plate up to near its top, a shallow panel on its outside, and the block on its inside
    that plugs into the Mega Drive's expansion port (hidden in it once it is on)."""
    plate = box(x, y, z, W / 2 - 1.5, W / 2 + 17, SEAM + 2, CONN_TOP, CONN_Z0, CONN_Z1, 8.0)
    d = cut(plate, box(x, y, z, W / 2 + 15.8, W / 2 + 22, SEAM + 10, CONN_TOP - 9, CONN_Z0 + 10, CONN_Z1 - 10, 4.0), 0.6)
    blk = box(x, y, z, CONN_X, W / 2, TOP - 1, TOP + 18, CONN_Z0 + 16, CONN_Z1 - 16, 1.5)
    d = smin(d, blk, 1.0)
    d = cut(d, box(x, y, z, CONN_X - 2, CONN_X + 7, TOP + 6, TOP + 11, CONN_Z0 + 20, CONN_Z1 - 20, 0.5), 0.2)
    return d


def front_printing(g, add):
    zp = FRONT - 0.4  # the panel's face
    # The glossy band with the door's opening in it.
    band = stadium(*PANEL).difference(sg.box(DOOR[0] - 0.4, DOOR[1] - 0.4, DOOR[2] + 0.4, DOOR[3] + 0.4))
    add(polygon_mesh(band, zp - 1.4, zp), GLOSS, "panel", 0.08)
    zt = zp
    # SEGA under the door, the gold Mega-CD player badge right of it.
    add(logo_mesh(LOGOS["sega"], -112.0, 21.6, 4.6, zt, depth=0.12), PRINT, "sega", 0.5)
    # The badge: MEGA (traced), two discs flying right with C and D on them, PLAYER.
    badge = [traced("badge", zt, zt + 0.12, keep=lambda p: p.centroid.y > 24.3 and p.centroid.x < 20)]
    discs, boxes = [], []
    for cx, cy, letter in ((13.6, 21.0, "C"), (21.6, 23.4, "D")):
        e = sg.Point(0, 0).buffer(1, quad_segs=24)
        e = affinity.rotate(affinity.scale(e, 6.4, 2.5), 22, origin=(0, 0))
        discs.append(affinity.translate(e, cx, cy))
        boxes.append((sg.box(cx - 1.6, cy - 1.7, cx + 1.6, cy + 1.7), letter, cx, cy))
    gold = sg.MultiPolygon(discs).buffer(0).difference(sg.MultiPolygon([b for b, _, _, _ in boxes]).buffer(0))
    badge.append(polygon_mesh(gold, zt, zt + 0.12))
    badge.append(text_mesh("PLAYER", FONT_ITALIC, 1.0, 22.0, 18.9, zt, 0.12))
    for b, letter, cx, cy in boxes:
        badge.append(polygon_mesh(b.exterior.buffer(0.18), zt, zt + 0.12))
        badge.append(text_mesh(letter, FONT_BOLD, 2.4, cx, cy, zt, 0.12))
    add(merge(badge), GOLD, "badge", 0.35)
    # The lights' box: its outline, READY and ACCESS, the Compact Disc mark.
    outline = sg.box(36.2, 30.8, 132.5, 47.2).buffer(2.5).exterior.buffer(0.22)
    lines = [polygon_mesh(outline, zt, zt + 0.1)]
    legend = sg.box(35.6, 19.1, 119.9, 26.2).buffer(2.2).exterior.buffer(0.2)
    lines.append(polygon_mesh(legend, zt, zt + 0.1))
    for lx in (56.4, 77.1, 98.7):
        lines.append(polygon_mesh(sg.box(lx - 0.18, 17.6, lx + 0.18, 27.6), zt, zt + 0.1))
    add(merge(lines), LINE, "lines", 0.5)
    for name, (x0, y0, x1, y1) in (("ready", READY), ("access", ACCESS)):
        add(polygon_mesh(sg.box(x0, y0, x1, y1).buffer(0.8).buffer(-0.8), zt, zt + 0.15), WINDOW, name, 0.3)
    labels = [traced("ready", zt + 0.15, zt + 0.75), traced("access", zt + 0.15, zt + 0.75)]
    add(merge(labels), (0.015, 0.015, 0.02), "labels", 0.4)
    cd = [traced("disc", zt, zt + 0.12)]
    printed = cd + [
        text_mesh("CD", FONT_REGULAR, 1.3, 45.6, 25.0, zt, 0.1),
        text_mesh("ACCESS", FONT_REGULAR, 1.3, 45.6, 23.2, zt, 0.1),
        text_mesh("CD IN", FONT_REGULAR, 1.3, 66.6, 24.6, zt, 0.1),
        text_mesh("CD OUT", FONT_REGULAR, 1.3, 88.0, 24.6, zt, 0.1),
        text_mesh("STAND-BY", FONT_REGULAR, 1.3, 109.4, 25.0, zt, 0.1),
        text_mesh("MODE", FONT_REGULAR, 1.3, 109.4, 23.2, zt, 0.1),
    ]
    # The little lines between each pair of lights.
    for x0, x1 in ((42.9, 47.4), (63.2, 66.0), (85.2, 89.0), (106.8, 109.6)):
        printed.append(polygon_mesh(sg.box(x0 + 0.4, 20.9, x1 - 0.4, 21.15), zt, zt + 0.1))
    add(merge(printed), PRINT, "printing", 0.5)
    greens = [(39.3, 42.9), (60.0, 63.2), (81.6, 85.2), (101.4, 106.8)]
    reds = [(47.4, 51.3), (89.0, 94.0)]
    add(merge([polygon_mesh(sg.box(a, 20.4, b, 21.6), zt, zt + 0.15) for a, b in greens]), GREEN, "green marks", 0.4)
    add(merge([polygon_mesh(sg.box(a, 20.4, b, 21.6), zt, zt + 0.15) for a, b in reds]), RED, "red marks", 0.4)


def back_parts(add):
    """The sockets' rings and the printing on the back: made facing +z at -x (turn() puts
    them at x, facing the back)."""
    zb = D / 2 - 3.5
    for sx, colour in ((-50.5, RCA_RED), (-66.5, RCA_WHITE)):
        ring = sg.Point(-sx, 21.5).buffer(3.3).difference(sg.Point(-sx, 21.5).buffer(1.6))
        add(turn(polygon_mesh(ring, zb + 0.6, zb + 3.4)), colour, "socket", 0.4)
        add(turn(polygon_mesh(sg.Point(-sx, 21.5).buffer(1.6), zb + 3, zb + 6)), HOLE, "socket hole", 0.6)
    zt = D / 2 - 2.9
    small_print = [
        text_mesh("R", FONT_REGULAR, 1.6, 50.5, 12.2, zt, 0.1),
        text_mesh("LINE OUT", FONT_REGULAR, 1.1, 58.5, 12.2, zt, 0.1),
        text_mesh("L", FONT_REGULAR, 1.6, 66.5, 12.2, zt, 0.1),
        text_mesh("MIXING", FONT_REGULAR, 1.1, 81.5, 27.5, zt, 0.1),
        text_mesh("DC 9V", FONT_REGULAR, 1.1, 96.0, 12.2, zt, 0.1),
    ]
    add(turn(merge(small_print)), PRINT, "back printing", 0.5)


def turn(part):
    """A part turned half round the vertical axis: the front's to the back."""
    v, f, n = part
    v, n = v.copy(), n.copy()
    v[:, 0], v[:, 2] = -v[:, 0], -v[:, 2]
    n[:, 0], n[:, 2] = -n[:, 0], -n[:, 2]
    return v, f, n


def traced(name, z0, z1, keep=None):
    """A logo traced from the photos, at its place on the front."""
    ps = [sg.Polygon(o, h) for o, h in PRINTED[name]]
    if keep:
        ps = [p for p in ps if keep(p)]
    return polygon_mesh(sg.MultiPolygon(ps).buffer(0), z0, z1)


def console():
    g = Glb()
    to_m = SCALE / 1000.0

    def add(part, colour, name, shine=0.5, colours=None):
        v, f, n = part
        g.add(v * to_m, f, n, g.material(name, colour, roughness=shine), colours=colours)

    print("mega-cd: body")
    v, f, n = mesh(body, (-W / 2, 0, -D / 2), (W / 2, TOP, D / 2), 0.45, keep=0.07)
    g.add(v * to_m, f, n, g.material("body", (1, 1, 1), roughness=0.45), colours=body_colour(v))
    print("mega-cd: connector and lock")
    add(small(connector, (CONN_X - 3, SEAM, CONN_Z0 - 2), (W / 2 + 19, CONN_TOP + 1, CONN_Z1 + 2), 0.35, 0.1), BLACK, "connector", 0.4)
    add(small(lock, (-W / 2 - 1, 38, 84), (-W / 2 + 5, 56, 104), 0.25, 0.2), BLACK, "lock", 0.4)
    pins = []
    for i in range(30):
        pz = CONN_Z0 + 21.5 + i * (CONN_Z1 - CONN_Z0 - 43) / 29
        for py in (TOP + 7.3, TOP + 9.7):
            pins.append(polygon_mesh(sg.box(pz - 0.45, py - 0.35, pz + 0.45, py + 0.35), 0, 1))
    pv, pf, pn = merge(pins)
    # Made in (z, y) facing +z: turned to face -x, at the block's mouth.
    pv = np.stack([CONN_X + 5 - pv[:, 2] * 3, pv[:, 1], pv[:, 0]], axis=1)
    pn = np.stack([-pn[:, 2], pn[:, 1], pn[:, 0]], axis=1)
    add((pv, pf[:, ::-1].copy(), pn), PIN, "pins", 0.25)
    print("mega-cd: front")
    front_printing(g, add)
    back_parts(add)
    path = os.path.join(OUT, "megacd.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")


# The tray: the door, then the bed of the disc behind it.
TRAY_W, TRAY_D = 142.0, 170.0
TRAY_BED = 38.0  # the top of the bed, where the disc lies
DISC_Z = FRONT - 3.5 - 85.0  # the disc (12 cm, 128.6 of these mm) in its bed


def tray(x, y, z):
    cx = (DOOR[0] + DOOR[2]) / 2
    plate = box(x, y, z, cx - TRAY_W / 2, cx + TRAY_W / 2, 33, TRAY_BED + 1.2, FRONT - 2.5 - TRAY_D, FRONT - 2.5, 1.0)
    d = cut(plate, extrude(circle(x, z, cx, DISC_Z, 65.5), y, TRAY_BED, TRAY_BED + 5), 0.4)
    # The middle, open for the spindle, and the slot the laser reads through.
    d = cut(d, extrude(circle(x, z, cx, DISC_Z, 11), y, 30, 45), 0.3)
    d = cut(d, box(x, y, z, cx - 10, cx + 10, 30, 45, DISC_Z - 60, DISC_Z - 12, 3), 0.3)
    door = box(x, y, z, DOOR[0], DOOR[2], DOOR[1], DOOR[3], FRONT - 3.5, FRONT + 0.6, 1.2)
    return smin(d, door, 0.5)


def tray_model():
    g = Glb()
    to_m = SCALE / 1000.0
    print("tray")
    cx = (DOOR[0] + DOOR[2]) / 2
    lo = (cx - TRAY_W / 2 - 1, DOOR[1] - 1, FRONT - 3.5 - TRAY_D)
    hi = (cx + TRAY_W / 2 + 1, DOOR[3] + 1, FRONT + 1)
    v, f, n = mesh(tray, lo, hi, 0.3, keep=0.1)
    g.add(v * to_m, f, n, g.material("tray", GLOSS, roughness=0.15))
    # The door's thin frame and MEGA-CD across it.
    zf = FRONT + 0.6
    frame = sg.box(DOOR[0] + 1.8, DOOR[1] + 1.8, DOOR[2] - 1.8, DOOR[3] - 1.8).exterior.buffer(0.25)
    v, f, n = polygon_mesh(frame, zf, zf + 0.08)
    g.add(v * to_m, f, n, g.material("door line", (0.06, 0.06, 0.07), roughness=0.3))
    v, f, n = traced("megacd", zf, zf + 0.12)
    g.add(v * to_m, f, n, g.material("title", PRINT, roughness=0.5))
    path = os.path.join(OUT, "megacd-tray.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")
    print("tray bounds x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f, disc at x %.1f y %.1f z %.1f" % (
        lo[0] + 1, hi[0] - 1, DOOR[1], DOOR[3], lo[2] + 0, FRONT + 0.6, cx, TRAY_BED, DISC_Z))


if __name__ == "__main__":
    which = sys.argv[1:] or ["console", "tray"]
    if "console" in which:
        console()
    if "tray" in which:
        tray_model()
