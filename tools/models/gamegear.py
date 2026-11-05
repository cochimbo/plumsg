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
from gamegear_front import FRONT_SHAPES, LOGOS  # noqa: E402
from sdf import box, circle, cut, extrude, flat_normals, mesh, round_rect, smax, smin  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "assets", "models")
FONT_BOLD = "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf"
FONT_REGULAR = "/usr/share/fonts/truetype/freefont/FreeSans.ttf"
FONT_ITALIC = "/usr/share/fonts/truetype/freefont/FreeSansBoldOblique.ttf"
CREDIT = "plumsg (made by tools/models/gamegear.py)"

# The console's depth: its front face, the back between the grips and the grips' backs.
FRONT, BACK, GRIP = 16.5, -15.0, -20.0
TOP = 110.0  # the top of the raised middle, where the slot is
SLOT_Z0, SLOT_Z1, SLOT_HALF = -12.3, -1.2, 35.5
JACK_X, WHEEL_X = 44.0, 66.0
SW_X = 68.0  # the power switch, on top at the right, with its red tab
RED = (0.75, 0.04, 0.04)
# Buttons 1 and 2: middle and radius, from the traced front.
BUTTONS = [(72.45, 58.95, 6.05), (88.2, 71.0, 6.0)]

# The plastics, as the shelf's light shows them (linear colours, like console_models.cpp's).
CHARCOAL = (0.15, 0.15, 0.158)
BLACK = (0.1, 0.1, 0.106)
HOLE = (0.008, 0.008, 0.01)
GLOSS = (0.075, 0.075, 0.08)
WINDOW = (0.05, 0.05, 0.055)
GREY = (0.085, 0.085, 0.095)
PAD = (0.1, 0.1, 0.106)
BLUE = (0.1, 0.26, 0.8)
PRINT = (0.78, 0.78, 0.8)
TEAL = (0.12, 0.48, 0.4)
LOGO_GREY = (0.62, 0.62, 0.64)
LENS = (0.25, 0.03, 0.03)
STRIPES = [(0.8, 0.03, 0.03), (0.0, 0.6, 0.38), (0.0, 0.42, 0.95)]
BOARD = (0.06, 0.24, 0.12)
GOLD = (0.8, 0.62, 0.24)


class Field:
    """A plane shape's signed distance (mm), from a raster of it."""

    RES = 0.08
    X0, Y0, X1, Y1 = -110.0, -6.0, 110.0, 116.0

    def __init__(self, polygon):
        from scipy import ndimage

        w = int((self.X1 - self.X0) / self.RES) + 1
        h = int((self.Y1 - self.Y0) / self.RES) + 1
        img = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(img)
        polys = polygon.geoms if hasattr(polygon, "geoms") else [polygon]
        for p in polys:
            d.polygon([((x - self.X0) / self.RES, (y - self.Y0) / self.RES) for x, y in p.exterior.coords], fill=1)
            for hole in p.interiors:
                d.polygon([((x - self.X0) / self.RES, (y - self.Y0) / self.RES) for x, y in hole.coords], fill=0)
        inside = np.asarray(img, bool)
        d = (ndimage.distance_transform_edt(~inside) - ndimage.distance_transform_edt(inside)) * self.RES
        # Smoothed a little, so the raster's steps do not ripple the rounded edges.
        self.d = ndimage.gaussian_filter(d, 2.5).astype(np.float32)

    def __call__(self, x, y):
        from scipy import ndimage

        shape = np.shape(x)
        coords = [((np.ravel(y) - self.Y0) / self.RES), ((np.ravel(x) - self.X0) / self.RES)]
        return ndimage.map_coordinates(self.d, coords, order=1, mode="nearest").reshape(shape)


def shape(name):
    return sg.Polygon(FRONT_SHAPES[name]).buffer(0)


def fields():
    f = {}
    f["outline"] = Field(shape("outline").union(shape("panel").intersection(sg.box(-80, 60, 80, TOP))))
    f["panel"] = Field(shape("panel"))
    f["bezel"] = Field(shape("bezel"))
    f["dpad_well"] = Field(shape("dpad_well"))
    f["pod"] = Field(shape("pod"))
    f["grip_line"] = Field(shape("grip_line"))
    f["buttons"] = Field(shape("button1").buffer(0.9).union(shape("button2").buffer(0.9)))
    f["start"] = Field(shape("start"))
    return f


F = {}


def speaker_holes():
    """The speaker's grid of small round holes, low on the left under the pad."""
    holes = []
    for row in range(7):
        n = 8 - abs(row - 3) // 2
        for col in range(n):
            holes.append((-86.5 - 1.9 * (n - 1) / 2 + 1.9 * col + (0.95 if row % 2 else 0), 13.0 + 1.65 * row))
    return holes


def body(x, y, z):
    ax = np.abs(x)
    d2 = F["outline"](x, y)
    d = extrude(d2, z, BACK, FRONT, 5.0)
    # The front's bottom edge rounds over much more softly than the others.
    chin = np.sqrt(np.maximum(11 - y, 0) ** 2 + np.maximum(z - (FRONT - 11), 0) ** 2) - 11
    d = smax(d, chin, 2)
    # The grips' backs, where the six AA batteries go, bulging out behind.
    grip = extrude(np.maximum(np.maximum(d2 + 4.0, 34 - ax), y - 96), z, GRIP, 0, 12)
    d = smin(d, grip, 7)
    # The battery covers' outlines on the back.
    groove = np.abs(round_rect(ax, y, 70, 52, 26, 40, 20)) - 0.4
    d = cut(d, extrude(groove, z, GRIP - 3, GRIP + 1.3), 0.3)
    # The middle panel stands a little proud of the front, the pad sits in its round well,
    # a line runs round the pad's grip, and 1 and 2 sit on their own raised pod.
    d = smin(d, extrude(F["panel"](x, y), z, FRONT - 3, FRONT + 0.7, 0.35), 0.4)
    d = cut(d, extrude(F["dpad_well"](x, y), z, FRONT - 2.2, FRONT + 5), 0.5)
    d = cut(d, extrude(F["grip_line"](x, y) - 0.15, z, FRONT - 0.6, FRONT + 5), 0.2)
    pod = extrude(F["pod"](x, y), z, FRONT - 3, FRONT + 1.0, 0.8)
    d = smin(d, pod, 0.6)
    # 1 and 2 each sit in a raised collar.
    for c in BUTTONS:
        r = np.sqrt((x - c[0]) ** 2 + (y - c[1]) ** 2)
        d = smin(d, extrude(r - c[2] - 1.6, z, FRONT - 1, FRONT + 2.0, 0.9), 0.6)
        d = cut(d, extrude(r - c[2] - 0.3, z, FRONT + 0.2, FRONT + 6), 0.3)
    d = cut(d, extrude(F["start"](x, y) - 0.3, z, FRONT - 0.5, FRONT + 5), 0.3)
    for hx, hy in speaker_holes():
        d = cut(d, extrude(circle(x, y, hx, hy, 0.5), z, FRONT - 3, FRONT + 5), 0.1)
    # On top: the cartridge slot in the raised middle, the power switch's groove at its left,
    # the link port at the right.
    d = cut(d, box(x, y, z, -SLOT_HALF, SLOT_HALF, TOP - 44, TOP + 8, SLOT_Z0, SLOT_Z1, 0.5), 0.8)
    d = smin(d, box(x, y, z, -60, -43, 104, 108.6, -3.2, 3.2, 1.0), 0.6)
    d = cut(d, box(x, y, z, SW_X - 6.5, SW_X + 6.5, 104.5, TOP + 8, -2.6, 2.6, 0.8), 0.4)
    # The link port, a slit in the right end.
    d = cut(d, box(x, y, z, W / 2 - 4, W / 2 + 5, 66, 90, -2.2, 2.2, 0.8), 0.4)
    # Underneath: the headphone socket and the volume wheel's opening; the adaptor's
    # socket on the left end.
    d = cut(d, extrude(np.sqrt((x - JACK_X) ** 2 + z ** 2) - 2.3, y, -5, 7), 0.4)
    d = cut(d, box(x, y, z, WHEEL_X - 7, WHEEL_X + 7, -5, 3, -2.4, 2.4, 0.6), 0.4)
    d = cut(d, extrude(np.sqrt((y - 52) ** 2 + z ** 2) - 3.2, x, -W / 2 - 5, -W / 2 + 7), 0.4)
    return d


W = 210.0


def body_colour(v):
    """Charcoal, near black in the openings."""
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    c = np.tile(np.array(CHARCOAL, np.float32), (len(v), 1))
    hole = (np.abs(x) < SLOT_HALF + 0.5) & (y > TOP - 43) & (y < TOP - 1.5) & (z > SLOT_Z0 - 0.5) & (z < SLOT_Z1 + 0.5)
    hole |= (y < 5.5) & ((np.sqrt((x - JACK_X) ** 2 + z ** 2) < 2.4) | ((np.abs(x - WHEEL_X) < 7.2) & (np.abs(z) < 2.6)))
    hole |= (x > W / 2 - 4.5) & (y > 65.5) & (y < 90.5) & (np.abs(z) < 2.4)
    hole |= (x < -74) & (y < 25) & (z > FRONT - 3.2) & (z < FRONT - 0.6)  # the speaker's holes
    c[hole] = HOLE
    return c


def text_mesh(s, font, size, cx, cy, z, depth=0.1, anchor="middle"):
    """Printed (barely raised) text: `size` is its capitals' height in mm."""
    path = TextPath((0, 0), s, size=10, prop=FontProperties(fname=font))
    shp = None
    for ring in path.to_polygons():
        if len(ring) < 3:
            continue
        p = sg.Polygon(ring).buffer(0)
        shp = p if shp is None else shp.symmetric_difference(p)
    capital = TextPath((0, 0), "H", size=10, prop=FontProperties(fname=font)).get_extents().height
    k = size / capital
    minx, miny, maxx, maxy = shp.bounds
    ox = cx - (minx + maxx) / 2 * k if anchor == "middle" else cx - minx * k
    return polygon_mesh(shp, z, z + depth, k, ox, cy - size / 2)


def logo_mesh(polys, x0, y0, height, z, ref=None, depth=0.1):
    """A logo's polygons (outline, holes), scaled to `height` mm (that of `ref`, if given)
    with its bottom left corner at (x0, y0)."""
    def build(ps):
        return sg.MultiPolygon([sg.Polygon(o, h) for o, h in ps]).buffer(0)

    shp = build(polys)
    bx = build(ref).bounds if ref else shp.bounds
    k = height / (bx[3] - bx[1])
    return polygon_mesh(shp, z, z + depth, k, x0 - shp.bounds[0] * k, y0 - bx[1] * k)


def polygon_mesh(shp, z0, z1, k=1.0, ox=0.0, oy=0.0):
    polys = shp.geoms if hasattr(shp, "geoms") else [shp]
    vs, fs, off = [], [], 0
    for p in polys:
        if p.area <= 0:
            continue
        m = trimesh.creation.extrude_polygon(p, (z1 - z0) / k)
        v = m.vertices * k
        v[:, 0] += ox
        v[:, 1] += oy
        v[:, 2] += z0
        vs.append(v)
        fs.append(m.faces + off)
        off += len(v)
    return flat_normals(np.vstack(vs), np.vstack(fs))


def plate(x0, y0, x1, y1, r, z0, z1):
    """A flat rounded plate, with sharp edges and few triangles."""
    shp = sg.box(x0 + r, y0 + r, x1 - r, y1 - r).buffer(r, quad_segs=8) if r > 0 else sg.box(x0, y0, x1, y1)
    return polygon_mesh(shp, z0, z1)


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
    F.update(fields())
    g = Glb()
    to_m = 1 / 1000.0

    def add(part, colour, name, shine=0.5, colours=None):
        v, f, n = part
        g.add(v * to_m, f, n, g.material(name, colour, roughness=shine), colours=colours)

    print("console: shell")
    v, f, n = mesh(body, (-W / 2, 0, GRIP), (W / 2, TOP, FRONT + 1.2), 0.3, keep=0.08)
    g.add(v * to_m, f, n, g.material("shell", (1, 1, 1), roughness=0.6), colours=body_colour(v))

    print("console: front parts")
    zb = FRONT + 0.7
    # The glossy black glass over the middle, the screen's frame in it.
    add(polygon_mesh(shape("bezel").buffer(-0.2), zb - 0.5, zb + 0.4), GLOSS, "glass", 0.1)
    add(polygon_mesh(shape("window"), zb + 0.4, zb + 0.5), WINDOW, "screen frame", 0.2)
    # The pad: a round rocker with the cross standing on it and its four marks.
    dpad = shape("dpad")
    cx, cy = dpad.centroid.x, dpad.centroid.y
    rdisc = (dpad.bounds[2] - dpad.bounds[0]) / 2
    cross = shape("arm_up").union(shape("arm_down")).union(shape("arm_left")).union(shape("arm_right"))
    cross = cross.union(sg.box(cx - 4.6, cy - 4.6, cx + 4.6, cy + 4.6)).buffer(0.4).buffer(-0.4)
    fc = Field(cross)
    marks = Field(shape("tick1").union(shape("tick2")).union(shape("tick3")).union(shape("tick4")))

    def pad(x, y, z):
        r2 = (x - cx) ** 2 + (y - cy) ** 2
        r = np.sqrt(r2)
        # A shallow dish, its rim standing out, the cross pressed a little out of it.
        top = FRONT + 0.9 + 0.0045 * r2 - 0.0002 * r2 * r2 / 10
        disc = extrude(r - rdisc + 0.3, z, FRONT - 2.2, top, 0.7)
        arms = extrude(fc(x, y), z, FRONT - 2, top + 0.55, 0.35)
        d = smin(disc, arms, 0.3)
        d = cut(d, extrude(marks(x, y), z, FRONT, FRONT + 4), 0.15)
        return d

    add(small(pad, (cx - rdisc - 1, cy - rdisc - 1, FRONT - 2.4), (cx + rdisc + 1, cy + rdisc + 1, FRONT + 3)), PAD, "pad", 0.45)
    for name in ("button1", "button2"):
        b = shape(name)
        bx, by = b.centroid.x, b.centroid.y
        rb = (b.bounds[2] - b.bounds[0]) / 2

        def button(x, y, z, bx=bx, by=by, rb=rb):
            # Dished on top, its edge rounded, standing a little out of its collar.
            r = np.sqrt((x - bx) ** 2 + (y - by) ** 2)
            return extrude(r - rb + 0.6, z, FRONT, FRONT + 2.3 + 0.022 * r * r, 0.9)

        add(small(button, (bx - rb - 1, by - rb - 1, FRONT - 0.2), (bx + rb + 1, by + rb + 1, FRONT + 3.6)), BLACK, "button", 0.3)
    # Start: a blue half moon filling its notch in the panel's edge.
    st = shape("start").buffer(-0.7)
    sx, sy = st.centroid.x, st.centroid.y
    fs = Field(st)

    def start(x, y, z):
        r2 = (x - sx) ** 2 + (y - sy) ** 2
        return extrude(fs(x, y), z, FRONT - 0.5, FRONT + 2.0 - 0.012 * r2, 0.9)

    lo, hi = st.bounds[:2], st.bounds[2:]
    add(small(start, (lo[0] - 1, lo[1] - 1, FRONT - 0.7), (hi[0] + 1, hi[1] + 1, FRONT + 2.2)), BLUE, "start", 0.25)
    led = shape("led")
    add(polygon_mesh(led, zb + 0.4, zb + 0.55), LENS, "power light", 0.1)

    print("console: printing")
    zt = zb + 0.4
    # On the glass, left of the screen: the three ovals over GAME and GEAR, the power
    # light and POWER under them; right of it SEGA and "portable video game system".
    for i, colour in enumerate(STRIPES):
        add(logo_mesh(LOGOS["ovals"][i], -60.2 + 4.7 * i, 98.6, 4.6, zt, ref=LOGOS["ovals"][0]), colour, "oval", 0.5)
    printed = [
        logo_mesh(LOGOS["game"], -60.4, 93.2, 4.6, zt),
        logo_mesh(LOGOS["gear"], -60.4 + 1.3, 87.6, 4.6, zt),
        text_mesh("POWER", FONT_BOLD, 1.1, led.centroid.x, led.centroid.y - 2.8, zt),
    ]
    add(merge(printed), LOGO_GREY, "logo", 0.6)
    add(logo_mesh(LOGOS["sega"], 43.2, 94.2, 4.4, zt), LOGO_GREY, "sega", 0.6)
    small_print = [text_mesh(t, FONT_REGULAR, 1.35, 43.4, 91.2 - 2.1 * i, zt, anchor="left")
                   for i, t in enumerate(("PORTABLE", "VIDEO GAME", "SYSTEM"))]
    add(merge(small_print), TEAL, "portable", 0.6)
    # On the case: START beside its button, and 1 and 2 joined by a line over the buttons.
    b1, b2 = shape("button1").centroid, shape("button2").centroid
    line = sg.LineString([(b1.x - 6.2, b1.y + 7.6), (b1.x - 6.2, b1.y + 8.6), (b2.x - 2.0, b2.y + 10.6), (b2.x + 1.4, b2.y + 10.6)])
    case_print = [
        text_mesh("START", FONT_BOLD, 1.7, sx + 4.6, sy - 0.6, FRONT + 0.7, anchor="left"),
        text_mesh("1", FONT_BOLD, 2.0, b1.x - 6.0, b1.y + 6.2, FRONT + 0.7),
        text_mesh("2", FONT_BOLD, 2.0, b2.x + 2.6, b2.y + 9.8, FRONT + 0.7),
        polygon_mesh(line.buffer(0.22, cap_style="flat"), FRONT + 0.7, FRONT + 0.8),
    ]
    add(merge(case_print), PRINT, "printing", 0.6)

    print("console: switch and wheel")
    add(small(lambda x, y, z: box(x, y, z, SW_X - 5.5, SW_X - 0.5, 103.5, TOP - 0.2, -2.0, 2.0, 0.6),
              (SW_X - 7, 102, -3), (SW_X + 1, TOP + 1, 3)), GREY, "power switch", 0.4)
    add(small(lambda x, y, z: box(x, y, z, SW_X - 4.8, SW_X - 1.2, TOP - 0.6, TOP + 0.1, -1.2, 1.2, 0.3),
              (SW_X - 6, TOP - 1.5, -2), (SW_X, TOP + 1, 2), 0.08, 0.3), RED, "power mark", 0.4)

    def wheel(x, y, z):
        r = np.sqrt((x - WHEEL_X) ** 2 + (y - 5.2) ** 2)
        teeth = 0.25 * np.cos(np.arctan2(y - 5.2, x - WHEEL_X) * 40)
        return extrude(r - 5.0 - teeth, z, -1.9, 1.9, 0.4)

    add(small(wheel, (WHEEL_X - 6, 0, -2.2), (WHEEL_X + 6, 11, 2.2)), GREY, "volume", 0.4)

    path = os.path.join(OUT, "gamegear.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")
    lcd = shape("lcd").bounds
    print("screen: middle y %.2f, %.2f x %.2f mm, front at z %.2f" % ((lcd[1] + lcd[3]) / 2, lcd[2] - lcd[0], lcd[3] - lcd[1], zb + 0.5))
    print("power light at", led.centroid.x, led.centroid.y)


# The cartridge: 66 x 68 x 10 mm, as square as it looks, from photos of real ones. On the
# front a raised U along the top and down both sides holds the ridges to pull it out by
# and the label's top; under the label, "SEGA" in relief in a sunken pill. On the back a
# flat panel with "MADE IN JAPAN" on a plate, a small hole and the screw in its washer;
# the board's contacts at the bottom.
CW, CH, CD = 66.0, 68.0, 10.0
LABEL = (-27.0, -14.2, 27.0, 24.2)  # x0, y0, x1, y1 of its label, on the front
U_IN, U_TOP, U_END = 27.4, 31.2, 9.5  # the U's inside half width, its bar's underside, where its arms end
FACE = 0.9  # how far the rest of the front is below the U
PILL = (0.0, -23.6, 13.2, 4.1)  # the SEGA pill: middle, half sizes
SCREW = (0.0, -12.3)
SHELL = (0.085, 0.087, 0.093)
RELIEF = (0.13, 0.13, 0.138)
WASHER = (0.42, 0.42, 0.43)


def cartridge_shell(x, y, z):
    t = CD / 2
    d = box(x, y, z, -CW / 2, CW / 2, -CH / 2, CH / 2, -t, t, 0.9)
    # The notches on both sides where the U's arms end.
    for sx in (1, -1):
        d = cut(d, box(x, y, z, sx * CW / 2 - 1.0 if sx > 0 else -CW / 2 - 2, sx * CW / 2 + 2 if sx > 0 else -CW / 2 + 1.0,
                       U_END - 2.2, U_END, -t - 1, t + 1, 0.3), 0.3)
    # The front below the U.
    lower = smax(box(x, y, z, -CW, CW, -CH, U_END, t - FACE, t + 2), -box(x, y, z, -CW, -U_IN, -CH, U_END + 0.01, -t, t + 2), 0.01)
    lower = smax(lower, -box(x, y, z, U_IN, CW, -CH, U_END + 0.01, -t, t + 2), 0.01)
    inner = box(x, y, z, -U_IN, U_IN, -CH, CH, t - FACE, t + 2, 0.0)
    below_u = smin(lower, smax(inner, box(x, y, z, -CW, CW, -CH, U_TOP, -t, t + 2), 0.01), 0.01)
    d = cut(d, below_u, 0.35)
    # Inside the U, sunk further: the label's recess with its top corners round, and the
    # three ridges above it.
    lx0, ly0, lx1, ly1 = LABEL
    well = extrude(round_rect(x, y, 0, (ly0 - 0.4 + U_TOP) / 2, U_IN, (U_TOP - ly0 + 0.4) / 2, 3.2), z, t - 1.3, t + 2)
    d = cut(d, well, 0.25)
    for i in range(3):
        yy = 26.4 + 1.7 * i
        d = smin(d, box(x, y, z, -12.5, 12.5, yy - 0.42, yy + 0.42, t - 1.6, t - FACE - 0.1, 0.35), 0.15)
    # The pill under the label, sunk; its SEGA is a part of its own.
    px, py, hx, hy = PILL
    d = cut(d, extrude(round_rect(x, y, px, py, hx, hy, hy), z, t - FACE - 0.45, t + 2), 0.2)
    # The seam between the two halves, round the sides and the top.
    seam = smax(extrude(round_rect(x, y, 0, 0, CW / 2 + 1, CH / 2 + 1, 1), z, -1.75, -1.45), -box(x, y, z, -CW / 2 + 0.25, CW / 2 - 0.25, -CH / 2 + 0.25, CH / 2 - 0.25, -t, t), 0.01)
    d = cut(d, seam, 0.0)
    # The opening at the bottom for the contacts.
    d = cut(d, box(x, y, z, -26.5, 26.5, -CH / 2 - 2, -CH / 2 + 6.5, -3.4, 3.4, 0.4), 0.3)
    # The back: its panel a little sunk down to the band across the bottom, the plate,
    # the small hole and the screw's well.
    d = cut(d, box(x, y, z, -29.2, 29.2, -19.6, CH / 2 + 2, -t - 2, -t + 0.3, 0.3), 0.2)
    d = cut(d, box(x, y, z, -12.6, 12.6, 18.6, 22.8, -t - 2, -t + 0.5, 0.2), 0.1)
    d = smin(d, box(x, y, z, -12.0, 12.0, 19.1, 22.3, -t + 0.3, -t + 0.75, 0.1), 0.05)
    d = cut(d, extrude(circle(x, y, 0, 0.3, 0.6), z, -t - 2, -t + 2.5), 0.15)
    d = cut(d, extrude(circle(x, y, SCREW[0], SCREW[1], 3.9), z, -t - 2, -t + 1.4), 0.2)
    return d


def cartridge_washer(x, y, z):
    t = CD / 2
    ring = extrude(circle(x, y, SCREW[0], SCREW[1], 3.6), z, -t + 0.5, -t + 1.4, 0.3)
    return cut(ring, extrude(circle(x, y, SCREW[0], SCREW[1], 2.0), z, -t - 1, -t + 1.2), 0.2)


def cartridge_screw(x, y, z):
    t = CD / 2
    head = extrude(circle(x, y, SCREW[0], SCREW[1], 2.05), z, -t + 1.0, -t + 1.6, 0.4)
    slot_ = box(x, y, z, SCREW[0] - 1.4, SCREW[0] + 1.4, SCREW[1] - 0.25, SCREW[1] + 0.25, -t, -t + 1.2)
    slot2 = box(x, y, z, SCREW[0] - 0.25, SCREW[0] + 0.25, SCREW[1] - 1.4, SCREW[1] + 1.4, -t, -t + 1.2)
    return cut(cut(head, slot_, 0.1), slot2, 0.1)


def label_image():
    """The label a cartridge wears when a game has neither a scan nor a cover."""
    w, h = 540, 380
    im = Image.new("RGB", (w, h), (236, 236, 232))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w, 70], fill=(20, 24, 30))
    font = ImageFont.truetype(FONT_ITALIC, 44)
    d.text((w / 2, 36), "GAME GEAR", font=font, fill=(240, 240, 240), anchor="mm")
    d.rectangle([28, 96, w - 28, h - 28], outline=(30, 30, 36), width=4)
    return im


def mirrored(part):
    """A part made facing +z, turned to face -z (for the back)."""
    v, f, n = part
    v = v.copy()
    v[:, 0] = -v[:, 0]
    v[:, 2] = -v[:, 2]
    n = n.copy()
    n[:, 0] = -n[:, 0]
    n[:, 2] = -n[:, 2]
    return v, f, n


def cartridge():
    g = Glb()
    to_m = 1 / 1000.0
    t = CD / 2
    far = lambda v: np.full((len(v), 2), 4.0, np.float32)  # noqa: E731  away from the label's 0..1
    print("cartridge: shell")
    v, f, n = mesh(cartridge_shell, (-CW / 2, -CH / 2, -t), (CW / 2, CH / 2, t), 0.15, keep=0.07)
    g.add(v * to_m, f, n, g.material("shell", SHELL, roughness=0.6), uvs=far(v))
    v, f, n = small(cartridge_washer, (-5, SCREW[1] - 5, -t - 0.5), (5, SCREW[1] + 5, -t + 2), 0.1, 0.3)
    g.add(v * to_m, f, n, g.material("washer", WASHER, roughness=0.5), uvs=far(v))
    v, f, n = small(cartridge_screw, (-3, SCREW[1] - 3, -t - 0.5), (3, SCREW[1] + 3, -t + 2), 0.08, 0.3)
    g.add(v * to_m, f, n, g.material("screw", BLACK, roughness=0.4), uvs=far(v))
    # SEGA in relief in its pill, and MADE IN JAPAN on the back's plate.
    px, py, hx, hy = PILL
    sega = LOGOS["sega"]
    shp = sg.MultiPolygon([sg.Polygon(o, h) for o, h in sega]).buffer(0)
    bx = shp.bounds
    height = 4.6
    width = (bx[2] - bx[0]) * height / (bx[3] - bx[1])
    zf = t - FACE - 0.45
    lv, lf, ln = logo_mesh(sega, px - width / 2, py - height / 2, height, zf - 0.05, depth=0.4)
    g.add(lv * to_m, lf, ln, g.material("sega", RELIEF, roughness=0.45), uvs=far(lv))
    mv, mf, mn = mirrored(text_mesh("MADE IN JAPAN", FONT_REGULAR, 2.1, 0, 20.7, t - 0.32, depth=0.22))
    g.add(mv * to_m, mf, mn, g.material("print", RELIEF, roughness=0.45), uvs=far(mv))
    # The label, in its recess: v runs up, as the shelf reads it.
    x0, y0, x1, y1 = LABEL
    z = t - 1.25
    lv = np.array([[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]], np.float32)
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
    tex = g.image(label_image().transpose(Image.FLIP_TOP_BOTTOM))
    g.add(lv * to_m, [[0, 1, 2], [0, 2, 3]], np.tile([0, 0, 1], (4, 1)), g.material("label", (1, 1, 1), tex, 0.4), uvs=uv)
    # The board and its contacts, both sides, 2 mm apart.
    board = plate(-25.8, -CH / 2 + 0.6, 25.8, -CH / 2 + 9, 0.3, -0.6, 0.6)
    g.add(board[0] * to_m, board[1], board[2], g.material("board", BOARD, roughness=0.6), uvs=far(board[0]))
    pads = []
    for i in range(23):
        cx = -22.0 + 2.0 * i
        for side in (1, -1):
            m = trimesh.creation.box(extents=(1.3, 5.2, 0.08))
            m.apply_translation((cx, -CH / 2 + 3.6, side * 0.63))
            pads.append(flat_normals(m.vertices, m.faces))
    pv, pf, pn = merge(pads)
    g.add(pv * to_m, pf, pn, g.material("contacts", GOLD, roughness=0.25), uvs=far(pv))
    path = os.path.join(OUT, "gamegear-cartridge.glb")
    g.save(path, CREDIT)
    print(path, os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    which = sys.argv[1:] or ["console", "cartridge"]
    if "console" in which:
        console()
    if "cartridge" in which:
        cartridge()
