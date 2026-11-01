"""Builds assets/models/gamegear.glb and gamegear-cartridge.glb: Sega's Game Gear (the
first model, MK-2100) and its cartridge, from their real measurements.

    python3 tools/models/gamegear.py

Needs numpy, scikit-image, fast-simplification, shapely, trimesh, matplotlib and
Pillow. Units are millimetres here and metres in the files. The console stands on its
bottom edge facing +z, the middle of its footprint at the origin; the cartridge stands
with its label towards +z and its middle at the origin.
"""

import os
import sys

import numpy as np
import shapely.geometry as sg
import trimesh
from matplotlib.font_manager import FontProperties
from matplotlib.textpath import TextPath
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(__file__))
from glb import Glb  # noqa: E402
from sdf import box, circle, cut, extrude, flat_normals, mesh, round_rect, smin  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "assets", "models")
FONT_BOLD = "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf"
FONT_ITALIC = "/usr/share/fonts/truetype/freefont/FreeSansBoldOblique.ttf"
CREDIT = "plumsg (made by tools/models/gamegear.py), CC0"

# Where things are, matching ConsoleLayout in src/render/console_models.h (13.5 cm = 1 there).
W, H = 210.0, 113.0           # the console's width and height
FRONT, BACK, GRIP = 19.0, -15.0, -19.0
SCREEN_Y, SCREEN_W, SCREEN_H = 67.5, 64.8, 48.6
SLOT_Z0, SLOT_Z1, SLOT_HALF = -12.3, -1.2, 35.5
DPAD = (-76.0, 57.0)
B1, B2 = (64.0, 50.0), (83.0, 61.0)
START = (79.0, 84.0)
LED = (-60.0, 94.0)
JACK_X, WHEEL_X = 44.0, 66.0

# The plastics, as the shelf's light shows them (linear colours, like console_models.cpp's).
BLACK = (0.042, 0.042, 0.048)
HOLE = (0.008, 0.008, 0.01)
BEZEL = (0.022, 0.022, 0.026)
GREY = (0.085, 0.085, 0.095)
PAD = (0.15, 0.15, 0.165)
NAVY = (0.10, 0.12, 0.36)
BLUE = (0.16, 0.28, 0.72)
PRINT = (0.72, 0.72, 0.74)
SEGA_BLUE = (0.38, 0.5, 0.86)
LENS = (0.25, 0.03, 0.03)
BOARD = (0.06, 0.24, 0.12)
GOLD = (0.8, 0.62, 0.24)


def body(x, y, z):
    ax = np.abs(x)
    # The shell, rounded at both ends where the hands hold it, and the backs of the grips
    # bulging out behind (the batteries are in them, three AA each side).
    d = extrude(round_rect(x, y, 0, H / 2, W / 2, H / 2, 46), z, BACK, FRONT, 6.5)
    grip = extrude(round_rect(ax, y, 68, H / 2, 37, H / 2 - 5, 38), z, GRIP, 0, 11)
    d = smin(d, grip, 7)
    # The battery covers' outlines on the back.
    groove = np.abs(round_rect(ax, y, 69, H / 2, 27, 40, 22)) - 0.4
    d = cut(d, extrude(groove, z, GRIP - 3, GRIP + 1.1), 0.3)
    # The pad's round hollow, the wells of 1, 2 and Start, the power light's window.
    d = cut(d, extrude(circle(x, y, *DPAD, 16.5), z, FRONT - 2.4, FRONT + 5), 1.2)
    for b in (B1, B2):
        d = cut(d, extrude(circle(x, y, *b, 7.4), z, FRONT - 1.4, FRONT + 5), 0.8)
    d = cut(d, extrude(round_rect(x, y, *START, 6.6, 3.6, 3.5), z, FRONT - 1.0, FRONT + 5), 0.6)
    d = cut(d, extrude(circle(x, y, *LED, 2.2), z, FRONT - 0.8, FRONT + 5), 0.3)
    # The speaker's slits, slanting, under the buttons.
    for i in range(6):
        cx, cy = 66.0 + 4.2 * i, 26.0
        u = (x - cx) * 0.5 + (y - cy) * 0.866
        v = -(x - cx) * 0.866 + (y - cy) * 0.5
        d = cut(d, extrude(round_rect(u, v, 0, 0, 6.5, 0.8, 0.8), z, FRONT - 3, FRONT + 5), 0.4)
    # On top: the cartridge slot behind the screen, the power switch's groove at the left,
    # the link port at the right.
    d = cut(d, box(x, y, z, -SLOT_HALF, SLOT_HALF, H - 42, H + 5, SLOT_Z0, SLOT_Z1, 0.5), 0.8)
    d = cut(d, box(x, y, z, -68, -48, H - 2.2, H + 5, -3.4, 3.4, 1.0), 0.5)
    d = cut(d, box(x, y, z, 50, 63, H - 5, H + 5, -2.8, 2.8, 0.6), 0.4)
    # Underneath: the headphone socket and the volume wheel's opening.
    jack = extrude(np.sqrt((x - JACK_X) ** 2 + z ** 2) - 2.3, y, -5, 7)
    d = cut(d, jack, 0.4)
    d = cut(d, box(x, y, z, WHEEL_X - 7, WHEEL_X + 7, -5, 3, -2.4, 2.4, 0.6), 0.4)
    # The adaptor's socket on the left end.
    d = cut(d, extrude(np.sqrt((y - 56) ** 2 + z ** 2) - 3.2, x, -W / 2 - 5, -W / 2 + 7), 0.4)
    return d


def body_colour(v):
    """Near black, darker in the openings."""
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    c = np.tile(np.array(BLACK, np.float32), (len(v), 1))
    inside = ((np.abs(x) < SLOT_HALF + 0.5) & (y > H - 41) & (y < H - 1.5) & (z > SLOT_Z0 - 0.5) & (z < SLOT_Z1 + 0.5))
    inside |= (y < 5.5) & ((np.sqrt((x - JACK_X) ** 2 + z ** 2) < 2.4) | ((np.abs(x - WHEEL_X) < 7.2) & (np.abs(z) < 2.6)))
    inside |= (y > H - 4.5) & (x > 49.5) & (x < 63.5) & (np.abs(z) < 3)
    inside |= (z > FRONT - 3.2) & (z < FRONT - 1.5)  # the speaker's slits' bottoms
    c[inside] = HOLE
    return c


def text_mesh(s, font, size, cx, cy, z, depth=0.12, anchor="middle"):
    """Printed (barely raised) text: `size` is its capitals' height in mm."""
    path = TextPath((0, 0), s, size=10, prop=FontProperties(fname=font))
    shape = None
    for ring in path.to_polygons():
        if len(ring) < 3:
            continue
        p = sg.Polygon(ring).buffer(0)
        shape = p if shape is None else shape.symmetric_difference(p)
    capital = TextPath((0, 0), "H", size=10, prop=FontProperties(fname=font)).get_extents().height
    k = size / capital
    minx, miny, maxx, maxy = shape.bounds
    ox = cx - (minx + maxx) / 2 * k if anchor == "middle" else cx - minx * k
    polys = shape.geoms if hasattr(shape, "geoms") else [shape]
    vs, fs, off = [], [], 0
    for p in polys:
        m = trimesh.creation.extrude_polygon(p, depth / k)
        v = m.vertices * k
        v[:, 0] += ox
        v[:, 1] += cy - size / 2
        v[:, 2] += z
        vs.append(v)
        fs.append(m.faces + off)
        off += len(v)
    return flat_normals(np.vstack(vs), np.vstack(fs))


def plate(x0, y0, x1, y1, r, z0, z1):
    """A flat rounded plate, with sharp edges and few triangles."""
    shape = sg.box(x0 + r, y0 + r, x1 - r, y1 - r).buffer(r, quad_segs=8) if r > 0 else sg.box(x0, y0, x1, y1)
    m = trimesh.creation.extrude_polygon(shape, z1 - z0)
    m.apply_translation((0, 0, z0))
    return flat_normals(m.vertices, m.faces)


def merge(parts):
    vs, fs, ns, off = [], [], [], 0
    for v, f, n in parts:
        vs.append(v)
        fs.append(f + off)
        ns.append(n)
        off += len(v)
    return np.vstack(vs), np.vstack(fs), np.vstack(ns)


def small(f, lo, hi, step=0.15, keep=0.15):
    return mesh(f, lo, hi, step, keep)


def console():
    g = Glb()
    to_m = 1 / 1000.0

    def add(part, colour, name, shine=0.5, colours=None):
        v, f, n = part
        g.add(v * to_m, f, n, g.material(name, colour, roughness=shine), colours=colours)

    print("console: shell")
    v, f, n = mesh(body, (-W / 2, 0, GRIP), (W / 2, H, FRONT), 0.3, keep=0.08)
    g.add(v * to_m, f, n, g.material("shell", (1, 1, 1), roughness=0.55), colours=body_colour(v))

    print("console: front parts")
    # The screen's glass plate, a little proud of the front, with the screen in it
    # (the shelf draws the lit picture over it).
    bezel = plate(-47, 21.5, 47, 107.5, 7, FRONT - 1, FRONT + 0.6)
    add(bezel, BEZEL, "bezel", 0.15)
    screen = plate(-SCREEN_W / 2 - 1.5, SCREEN_Y - SCREEN_H / 2 - 1.5, SCREEN_W / 2 + 1.5, SCREEN_Y + SCREEN_H / 2 + 1.5, 0.8,
                   FRONT + 0.5, FRONT + 0.66)
    add(screen, HOLE, "screen surround", 0.1)
    # The pad: a cross on a round rocker.
    def dpad(x, y, z):
        lx, ly = x - DPAD[0], y - DPAD[1]
        disc = extrude(circle(lx, ly, 0, 0, 13.5), z, FRONT - 2.4, FRONT - 1.0, 0.6)
        arms = smin(extrude(round_rect(lx, ly, 0, 0, 12.5, 4.4, 1.6), z, FRONT - 2, FRONT + 2.6 - 0.012 * (lx * lx + ly * ly) / 12, 0.7),
                    extrude(round_rect(lx, ly, 0, 0, 4.4, 12.5, 1.6), z, FRONT - 2, FRONT + 2.6 - 0.012 * (lx * lx + ly * ly) / 12, 0.7), 1.0)
        dimple = extrude(circle(lx, ly, 0, 0, 2.6), z, FRONT + 1.3, FRONT + 4, 0)
        return cut(smin(disc, arms, 0.8), dimple, 0.6)
    add(small(dpad, (DPAD[0] - 15, DPAD[1] - 15, FRONT - 2.6), (DPAD[0] + 15, DPAD[1] + 15, FRONT + 2.6)), PAD, "pad", 0.45)
    for b in (B1, B2):
        def button(x, y, z, b=b):
            r = np.sqrt((x - b[0]) ** 2 + (y - b[1]) ** 2)
            top = FRONT + 2.6 - 0.025 * r * r  # a low dome
            return extrude(r - 6.2, z, FRONT - 1.4, top, 1.0)
        add(small(button, (b[0] - 7, b[1] - 7, FRONT - 1.6), (b[0] + 7, b[1] + 7, FRONT + 2.8)), NAVY, "button", 0.3)
    start = small(lambda x, y, z: extrude(round_rect(x, y, *START, 5.4, 2.6, 2.6), z, FRONT - 1, FRONT + 1.0, 0.8),
                  (START[0] - 7, START[1] - 4, FRONT - 1.2), (START[0] + 7, START[1] + 4, FRONT + 1.2))
    add(start, BLUE, "start", 0.3)
    lens = small(lambda x, y, z: extrude(circle(x, y, *LED, 2.0), z, FRONT - 0.9, FRONT - 0.3, 0.3),
                 (LED[0] - 3, LED[1] - 3, FRONT - 1), (LED[0] + 3, LED[1] + 3, FRONT))
    add(lens, LENS, "power light", 0.1)

    print("console: printing")
    printed = [
        text_mesh("POWER", FONT_BOLD, 1.9, LED[0], LED[1] - 5.2, FRONT),
        text_mesh("START", FONT_BOLD, 1.9, START[0], START[1] - 6.2, FRONT),
        text_mesh("1", FONT_BOLD, 2.6, B1[0] - 3.5, B1[1] - 10.5, FRONT),
        text_mesh("2", FONT_BOLD, 2.6, B2[0] - 3.5, B2[1] - 10.5, FRONT),
        text_mesh("GAME GEAR", FONT_ITALIC, 5.2, 0, 31.0, FRONT + 0.6),
    ]
    add(merge(printed), PRINT, "printing", 0.6)
    add(text_mesh("SEGA", FONT_BOLD, 5.0, 0, 99.5, FRONT + 0.6), SEGA_BLUE, "logo", 0.6)
    # The thin rule under GAME GEAR.
    rule = plate(-27, 25.6, 27, 26.2, 0, FRONT + 0.55, FRONT + 0.72)
    add(rule, PRINT, "rule", 0.6)

    print("console: switch and wheel")
    knob = small(lambda x, y, z: box(x, y, z, -66, -59, H - 3, H + 1.2, -2.6, 2.6, 0.8),
                 (-67, H - 4, -3), (-58, H + 2, 3))
    add(knob, GREY, "power switch", 0.4)
    def wheel(x, y, z):
        r = np.sqrt((x - WHEEL_X) ** 2 + (y - 5.2) ** 2)
        teeth = 0.25 * np.cos(np.arctan2(y - 5.2, x - WHEEL_X) * 40)
        return extrude(r - 5.0 - teeth, z, -1.9, 1.9, 0.4)
    add(small(wheel, (WHEEL_X - 6, 0, -2.2), (WHEEL_X + 6, 11, 2.2)), GREY, "volume", 0.4)

    path = os.path.join(OUT, "gamegear.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")


# The cartridge: 66 x 68 x 10 mm; the lower part, narrower, goes into the console.
CW, CH, CD = 66.0, 68.0, 10.0
LABEL = (-27.0, -6.0, 27.0, 30.0)  # x0, y0, x1, y1 of its label, on the front


def cartridge_shell(x, y, z):
    t = CD / 2
    upper = extrude(round_rect(x, y, 0, 15, CW / 2, 19, 4.5), z, -t, t, 1.3)
    lower = extrude(round_rect(x, y, 0, -16.5, CW / 2 - 2, 17.5, 2.0), z, -t, t, 1.0)
    d = smin(upper, lower, 1.5)
    # The label's recess on the front, the opening at the bottom for the contacts.
    d = cut(d, box(x, y, z, LABEL[0] - 0.6, LABEL[2] + 0.6, LABEL[1] - 0.6, LABEL[3] + 0.6, t - 0.5, t + 2, 0.6), 0.25)
    d = cut(d, box(x, y, z, -26.5, 26.5, -CH / 2 - 2, -CH / 2 + 6.5, -3.4, 3.4, 0.4), 0.3)
    # Ridges across the back to hold it by, a groove across the front under the label.
    for i in range(6):
        yy = 18.0 + 2.6 * i
        d = smin(d, box(x, y, z, -25, 25, yy - 0.55, yy + 0.55, -t - 0.55, -t + 0.4, 0.4), 0.3)
    d = cut(d, box(x, y, z, -24, 24, -12.6, -11.4, t - 0.4, t + 2, 0.4), 0.2)
    # The screw's hole at the back.
    d = cut(d, extrude(circle(x, y, 0, -20, 1.8), z, -t - 2, -t + 1.5), 0.3)
    return d


def label_image():
    """The label a cartridge wears when a game has neither a scan nor a cover."""
    w, h = 540, 360
    im = Image.new("RGB", (w, h), (236, 236, 232))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w, 70], fill=(20, 24, 30))
    font = ImageFont.truetype(FONT_ITALIC, 44)
    d.text((w / 2, 36), "GAME GEAR", font=font, fill=(240, 240, 240), anchor="mm")
    d.rectangle([28, 96, w - 28, h - 28], outline=(30, 30, 36), width=4)
    return im


def cartridge():
    g = Glb()
    to_m = 1 / 1000.0
    print("cartridge: shell")
    v, f, n = mesh(cartridge_shell, (-CW / 2, -CH / 2, -CD / 2 - 0.6), (CW / 2, CH / 2, CD / 2), 0.2, keep=0.1)
    # Texture coordinates away from the label's (0..1), so only the label shows a game's.
    far = np.full((len(v), 2), 4.0, np.float32)
    g.add(v * to_m, f, n, g.material("shell", BLACK, roughness=0.55), uvs=far)
    # The label, in its recess: v runs up, as the shelf reads it.
    x0, y0, x1, y1 = LABEL
    z = CD / 2 - 0.45
    lv = np.array([[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]], np.float32)
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
    tex = g.image(label_image().transpose(Image.FLIP_TOP_BOTTOM))
    g.add(lv * to_m, [[0, 1, 2], [0, 2, 3]], np.tile([0, 0, 1], (4, 1)), g.material("label", (1, 1, 1), tex, 0.4), uvs=uv)
    # The board and its contacts, both sides, 2 mm apart.
    board = plate(-25.8, -CH / 2 + 0.6, 25.8, -CH / 2 + 9, 0.3, -0.6, 0.6)
    g.add(board[0] * to_m, board[1], board[2], g.material("board", BOARD, roughness=0.6), uvs=np.full((len(board[0]), 2), 4.0))
    pads = []
    for i in range(23):
        cx = -22.0 + 2.0 * i
        for side in (1, -1):
            m = trimesh.creation.box(extents=(1.3, 5.2, 0.08))
            m.apply_translation((cx, -CH / 2 + 3.6, side * 0.63))
            pads.append(flat_normals(m.vertices, m.faces))
    pv, pf, pn = merge(pads)
    g.add(pv * to_m, pf, pn, g.material("contacts", GOLD, roughness=0.25), uvs=np.full((len(pv), 2), 4.0))
    path = os.path.join(OUT, "gamegear-cartridge.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    which = sys.argv[1:] or ["console", "cartridge"]
    if "console" in which:
        console()
    if "cartridge" in which:
        cartridge()
