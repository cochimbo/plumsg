#include "console_models.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace plumsg {

namespace {

constexpr float kPi = 3.14159265f;

struct Colour {
    float r, g, b;
};

// The plastics and the rest, as their real colours look under the scene's light.
constexpr Colour kBlack{0.045f, 0.045f, 0.052f};
constexpr Colour kBlackSoft{0.065f, 0.065f, 0.075f};
constexpr Colour kHole{0.008f, 0.008f, 0.01f};
constexpr Colour kGroove{0.02f, 0.02f, 0.024f};
constexpr Colour kFlap{0.1f, 0.1f, 0.11f};
constexpr Colour kButton{0.12f, 0.12f, 0.135f};
constexpr Colour kGold{0.62f, 0.48f, 0.2f};
constexpr Colour kRed{0.72f, 0.06f, 0.05f};
constexpr Colour kSilver{0.55f, 0.56f, 0.6f};
constexpr Colour kGgBlue{0.12f, 0.15f, 0.42f};
constexpr Colour kGgStart{0.18f, 0.3f, 0.75f};

// Writes triangles in a frame that is either the world's (y up) or one turned to face
// the viewer, where the builder's y is the world's z (towards the viewer) and the
// builder's z is the world's -y; so the same shapes make the parts of a front.
class Builder {
public:
    explicit Builder(std::vector<float>& out) : out_(out) {}

    // Everything after this is moved by (x, y, z) and, if `front`, turned to face +z.
    void frame(float x, float y, float z, bool front) {
        ox_ = x, oy_ = y, oz_ = z;
        front_ = front;
    }

    // A rounded rectangle (w x d, corners of radius r) lofted through rings at heights
    // y, each pulled in by its inset: bevels, tapers and domes. Caps on the top ring and,
    // if asked, the bottom one.
    void loft(float cx, float cz, float w, float d, float r, std::initializer_list<std::pair<float, float>> rings,
              Colour c, bool bottom = false, int corner = 8) {
        const std::vector<std::pair<float, float>> ring(rings);
        corner = std::max(corner, 2);
        const int n = corner * 4;
        auto point = [&](size_t k, int i, float* px, float* pz, float* nx, float* nz) {
            const float inset = ring[k].second;
            const float hx = std::max(w / 2 - inset, 0.0005f), hz = std::max(d / 2 - inset, 0.0005f);
            const float rr = std::clamp(r - inset, 0.0005f, std::min(hx, hz));
            const int q = i / corner;
            const float a = (static_cast<float>(q) + static_cast<float>(i % corner) / static_cast<float>(corner - 1)) * kPi / 2;
            const float ccx = (q == 0 || q == 3) ? hx - rr : -(hx - rr), ccz = (q == 0 || q == 1) ? hz - rr : -(hz - rr);
            *nx = std::cos(a), *nz = std::sin(a);
            *px = cx + ccx + rr * *nx, *pz = cz + ccz + rr * *nz;
        };
        for (size_t k = 0; k + 1 < ring.size(); ++k) {
            const float y0 = ring[k].first, y1 = ring[k + 1].first, di = ring[k + 1].second - ring[k].second;
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                float ax, az, anx, anz, bx, bz, bnx, bnz, cx0, cz0, cx1, cz1, dummy;
                point(k, i, &ax, &az, &anx, &anz);
                point(k, j, &bx, &bz, &bnx, &bnz);
                point(k + 1, i, &cx0, &cz0, &dummy, &dummy);
                point(k + 1, j, &cx1, &cz1, &dummy, &dummy);
                // Outwards and up as much as the ring steps in.
                const float dy = y1 - y0;
                const float na[3] = {anx * dy, di, anz * dy}, nb[3] = {bnx * dy, di, bnz * dy};
                tri({ax, y0, az}, {bx, y0, bz}, {cx1, y1, cz1}, na, nb, nb, c);
                tri({ax, y0, az}, {cx1, y1, cz1}, {cx0, y1, cz0}, na, nb, na, c);
            }
        }
        cap(ring.size() - 1, n, point, ring.back().first, 1, c);
        if (bottom) cap(0, n, point, ring.front().first, -1, c);
    }

    // An upright box, w wide, d deep, from y0 to y1, its edges barely rounded.
    void box(float cx, float y0, float y1, float cz, float w, float d, Colour c) {
        loft(cx, cz, w, d, 0.004f, {{y0, 0}, {y1, 0}}, c, true, 2);
    }

    // A flat part on a surface facing up at height y: a slot, a button, a groove.
    void top(float cx, float cz, float y, float w, float d, float r, float raise, Colour c) {
        loft(cx, cz, w, d, r, {{y, 0}, {y + raise, std::min(raise * 0.3f, 0.004f)}}, c);
    }

    // The same on the front (facing +z at depth z): (cx, cy) is its middle.
    void front(float cx, float cy, float z, float w, float h, float r, float raise, Colour c) {
        const float x = ox_, y = oy_, zz = oz_;
        const bool f = front_;
        frame(x + cx, y + cy, zz + z, true);
        loft(0, 0, w, h, r, {{0, 0}, {raise, std::min(raise * 0.3f, 0.004f)}}, c);
        frame(x, y, zz, f);
    }

    // A prism along x from x0 to x1 whose cross-section is the convex polygon of (z, y)
    // points: the sloping parts of a body.
    void prismX(float x0, float x1, std::initializer_list<std::pair<float, float>> poly, Colour c) {
        const std::vector<std::pair<float, float>> p(poly);
        float mz = 0, my = 0;
        for (const auto& [z, y] : p) mz += z, my += y;
        mz /= static_cast<float>(p.size()), my /= static_cast<float>(p.size());
        for (size_t i = 0; i < p.size(); ++i) {
            const auto [z0, y0] = p[i];
            const auto [z1, y1] = p[(i + 1) % p.size()];
            float nz = y1 - y0, ny = -(z1 - z0);
            if (nz * ((z0 + z1) / 2 - mz) + ny * ((y0 + y1) / 2 - my) < 0) nz = -nz, ny = -ny;
            const float n[3] = {0, ny, nz};
            tri({x0, y0, z0}, {x1, y0, z0}, {x1, y1, z1}, n, n, n, c);
            tri({x0, y0, z0}, {x1, y1, z1}, {x0, y1, z1}, n, n, n, c);
            const float l[3] = {-1, 0, 0}, r[3] = {1, 0, 0};
            tri({x0, my, mz}, {x0, y0, z0}, {x0, y1, z1}, l, l, l, c);
            tri({x1, my, mz}, {x1, y0, z0}, {x1, y1, z1}, r, r, r, c);
        }
    }

    // A prism along z from z0 to z1 whose cross-section is the convex polygon of (x, y)
    // points: the outline of a cartridge.
    void prismZ(float z0, float z1, std::initializer_list<std::pair<float, float>> poly, Colour c) {
        const std::vector<std::pair<float, float>> p(poly);
        float mx = 0, my = 0;
        for (const auto& [x, y] : p) mx += x, my += y;
        mx /= static_cast<float>(p.size()), my /= static_cast<float>(p.size());
        for (size_t i = 0; i < p.size(); ++i) {
            const auto [x0, y0] = p[i];
            const auto [x1, y1] = p[(i + 1) % p.size()];
            float nx = y1 - y0, ny = -(x1 - x0);
            if (nx * ((x0 + x1) / 2 - mx) + ny * ((y0 + y1) / 2 - my) < 0) nx = -nx, ny = -ny;
            const float n[3] = {nx, ny, 0};
            tri({x0, y0, z0}, {x0, y0, z1}, {x1, y1, z1}, n, n, n, c);
            tri({x0, y0, z0}, {x1, y1, z1}, {x1, y1, z0}, n, n, n, c);
            const float back[3] = {0, 0, -1}, fwd[3] = {0, 0, 1};
            tri({mx, my, z0}, {x0, y0, z0}, {x1, y1, z0}, back, back, back, c);
            tri({mx, my, z1}, {x0, y0, z1}, {x1, y1, z1}, fwd, fwd, fwd, c);
        }
    }

    // A rectangle of a prism, from (x0, y0) to (x1, y1) and z0 to z1.
    void slab(float x0, float y0, float x1, float y1, float z0, float z1, Colour c) {
        prismZ(z0, z1, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, c);
    }

private:
    struct P {
        float x, y, z;
    };

    // A fan from the middle of ring k (of n points), facing up or down.
    template <typename Point>
    void cap(size_t k, int n, Point& point, float y, float up, Colour c) {
        const float normal[3] = {0, up, 0};
        std::vector<P> pts(static_cast<size_t>(n));
        P m{0, y, 0};
        for (int i = 0; i < n; ++i) {
            float px, pz, nx, nz;
            point(k, i, &px, &pz, &nx, &nz);
            pts[static_cast<size_t>(i)] = {px, y, pz};
            m.x += px, m.z += pz;
        }
        m.x /= static_cast<float>(n), m.z /= static_cast<float>(n);
        for (size_t i = 0; i < pts.size(); ++i) tri(m, pts[i], pts[(i + 1) % pts.size()], normal, normal, normal, c);
    }

    void tri(P a, P b, P c, const float* na, const float* nb, const float* nc, Colour col) {
        vertex(a, na, col);
        vertex(b, nb, col);
        vertex(c, nc, col);
    }

    void vertex(P p, const float* n, Colour c) {
        float x = p.x, y = p.y, z = p.z, nx = n[0], ny = n[1], nz = n[2];
        if (front_) {
            std::swap(y, z), std::swap(ny, nz);
            y = -y, ny = -ny;
        }
        const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 0) nx /= l, ny /= l, nz /= l;
        out_.insert(out_.end(), {x + ox_, y + oy_, z + oz_, nx, ny, nz, 0, 0, 0, c.r, c.g, c.b});
    }

    std::vector<float>& out_;
    float ox_ = 0, oy_ = 0, oz_ = 0;
    bool front_ = false;
};

// Rows of grooves on a surface facing up: `count` of them, w long and d apart, from (x, z).
void grooves(Builder& b, float x, float z, float y, int count, float w, float step, bool alongX) {
    for (int i = 0; i < count; ++i) {
        const float o = static_cast<float>(i) * step;
        if (alongX) b.top(x + w / 2, z + o, y, w, 0.014f, 0.005f, 0.002f, kGroove);
        else b.top(x + o, z + w / 2, y, 0.014f, w, 0.005f, 0.002f, kGroove);
    }
}

// The slot of a cartridge: the mouth and its two spring flaps, closed in the middle.
void slot(Builder& b, float x, float z, float y, float w, float d) {
    b.top(x, z, y, w + 0.04f, d + 0.04f, 0.03f, 0.004f, kBlackSoft);
    b.top(x, z, y + 0.004f, w, d, 0.02f, 0.002f, kHole);
    b.top(x, z - d * 0.26f, y + 0.006f, w * 0.97f, d * 0.44f, 0.006f, 0.002f, kFlap);
    b.top(x, z + d * 0.26f, y + 0.006f, w * 0.97f, d * 0.44f, 0.006f, 0.002f, kFlap);
}

void megaDrive(Builder& b) {
    using L = ConsoleLayout;
    // The body, bevelled along the top, and the deck at 0.30 (4 cm).
    b.loft(0, 0, 2.06f, 1.59f, 0.09f, {{0, 0.02f}, {0.015f, 0}, {0.25f, 0}, {0.285f, 0.03f}, {0.3f, 0.06f}}, kBlack);
    // The round top, 15.5 cm across, rising 1.4 cm in steps to its flat top.
    const float dx = L::kMdSlotX, dz = L::kMdSlotZ;
    b.loft(dx, dz, 1.16f, 1.16f, 0.58f, {{0.3f, 0}, {0.33f, 0.015f}, {0.36f, 0.05f}, {0.385f, 0.1f}, {0.4f, 0.15f}}, kBlack,
           false, 16);
    // The ring around the slot with "16-BIT" in gold on its back right, and the slot.
    b.top(dx, dz, 0.4f, 0.86f, 0.86f, 0.43f, 0.002f, kBlackSoft);
    b.top(dx, dz, 0.4f, 0.78f, 0.78f, 0.39f, 0.003f, kBlack);
    for (int i = 0; i < 6; ++i) {
        const float a = (25.0f + 9.0f * static_cast<float>(i)) * kPi / 180;
        b.top(dx + 0.41f * std::cos(a), dz - 0.41f * std::sin(a), 0.4f, 0.03f, 0.026f, 0.004f, 0.0025f, kGold);
    }
    slot(b, dx, dz, 0.4f, 0.74f, 0.15f);
    // The power switch on the top left and its light, the reset button on the right.
    b.top(-0.82f, 0.55f, 0.3f, 0.26f, 0.09f, 0.02f, 0.002f, kHole);
    b.loft(-0.88f, 0.55f, 0.08f, 0.07f, 0.012f, {{0.3f, 0}, {0.34f, 0.006f}}, kButton);
    b.top(L::kMdLed[0], L::kMdLed[2], 0.3f, 0.07f, 0.07f, 0.035f, 0.002f, kBlackSoft);
    b.loft(0.8f, 0.55f, 0.22f, 0.11f, 0.04f, {{0.3f, 0}, {0.325f, 0.004f}, {0.33f, 0.015f}}, kButton);
    // Vents along the back on both sides of the round top.
    grooves(b, -0.95f, -0.62f, 0.3f, 6, 0.42f, 0.05f, true);
    grooves(b, 0.62f, -0.62f, 0.3f, 6, 0.36f, 0.05f, true);
    // The front: the headphone socket and the volume slider on the left, the two
    // controller ports on the right.
    b.front(-0.86f, 0.14f, 0.795f, 0.05f, 0.05f, 0.025f, 0.004f, kHole);
    b.front(-0.62f, 0.14f, 0.795f, 0.22f, 0.03f, 0.012f, 0.003f, kHole);
    b.front(-0.66f, 0.14f, 0.795f, 0.04f, 0.05f, 0.008f, 0.012f, kButton);
    for (float x : {0.34f, 0.7f}) {
        b.front(x, 0.13f, 0.795f, 0.24f, 0.12f, 0.035f, 0.004f, kBlackSoft);
        b.front(x, 0.13f, 0.795f, 0.2f, 0.085f, 0.03f, 0.006f, kHole);
    }
}

void mega32x(Builder& b) {
    using L = ConsoleLayout;
    // The stem that goes into the Mega Drive's slot, then the mushroom's head, 21 x 10 cm.
    b.loft(0, 0, 0.82f, 0.16f, 0.02f, {{-0.12f, 0}, {0.2f, 0}}, kBlackSoft);
    b.loft(0, 0, 1.56f, 0.74f, 0.2f,
           {{0.12f, 0.3f}, {0.17f, 0.12f}, {0.24f, 0.02f}, {0.27f, 0}, {0.6f, 0}, {0.66f, 0.03f}, {0.71f, 0.08f},
            {0.74f, 0.14f}, {0.75f, 0.2f}},
           kBlack, false, 12);
    slot(b, 0, 0, L::k32xSlotY, 0.57f, 0.14f);
    // The "32X" badge on the front and the vents on its shoulders.
    b.front(0.0f, 0.46f, 0.37f, 0.4f, 0.13f, 0.03f, 0.004f, kRed);
    b.front(0.0f, 0.46f, 0.37f, 0.3f, 0.06f, 0.01f, 0.006f, kSilver);
    grooves(b, -0.7f, -0.2f, L::k32xSlotY, 6, 0.24f, 0.07f, false);
    grooves(b, 0.33f, -0.2f, L::k32xSlotY, 6, 0.24f, 0.07f, false);
}

void megaCd(Builder& b) {
    using L = ConsoleLayout;
    // 30 x 24 cm, a little wider and deeper than the Mega Drive that sits on it.
    b.loft(0, 0, 2.22f, 1.75f, 0.06f, {{0, 0.02f}, {0.015f, 0}, {0.48f, 0}, {L::kCdHeight, 0.02f}}, kBlack);
    // The front: the tray's mouth, the open/close button, the access and power lights
    // and the vents; a silver line across.
    const float z = 0.875f;
    b.front(L::kTrayX, L::kTrayY - 0.03f, z, 1.28f, 0.1f, 0.01f, 0.002f, kHole);
    b.front(0.9f, 0.3f, z, 0.18f, 0.07f, 0.02f, 0.012f, kButton);
    b.front(L::kCdLed[0] - 0.1f, L::kCdLed[1], z, 0.05f, 0.03f, 0.01f, 0.002f, kBlackSoft);
    b.front(0.0f, 0.45f, z, 2.1f, 0.008f, 0.002f, 0.002f, kSilver);
    for (int i = 0; i < 4; ++i) b.front(0.85f, 0.1f + 0.04f * static_cast<float>(i), z, 0.4f, 0.014f, 0.005f, 0.002f, kGroove);
}

void megaCdTray(Builder& b) {
    // 16 cm deep with the round bed of the disc and the lip that closes the mouth.
    b.loft(0, 0, 1.22f, 1.2f, 0.03f, {{-0.06f, 0}, {0, 0}}, kBlackSoft, true);
    b.top(0, 0, 0, 0.94f, 0.94f, 0.47f, 0.002f, kHole);
    b.top(0, 0, 0, 0.16f, 0.16f, 0.08f, 0.01f, kSilver);
    b.loft(0, 0.6f, 1.26f, 0.03f, 0.008f, {{-0.07f, 0}, {0.025f, 0}}, kBlack, true);
}

void masterSystem(Builder& b) {
    using L = ConsoleLayout;
    // 36.5 x 17 x 7 cm: the raised back with the slot, the lower front sloping to the
    // viewer, red lines along both.
    b.loft(0, -0.34f, 2.7f, 0.58f, 0.03f, {{0, 0}, {0.49f, 0}, {0.52f, 0.03f}}, kBlack);
    b.prismX(-1.33f, 1.33f, {{-0.06f, 0}, {0.63f, 0}, {0.63f, 0.18f}, {0.34f, 0.36f}, {-0.06f, 0.36f}}, kBlack);
    b.front(0, 0.455f, -0.05f, 2.6f, 0.03f, 0.004f, 0.003f, kRed);
    b.front(0, 0.15f, 0.63f, 2.62f, 0.025f, 0.004f, 0.003f, kRed);
    slot(b, L::kSmsSlotX, L::kSmsSlotZ, 0.52f, 0.83f, 0.14f);
    // The grid on the left of the front, the pause and reset buttons, the card slot.
    for (int i = 0; i < 4; ++i) b.top(-0.78f, 0.03f + 0.07f * static_cast<float>(i), 0.36f, 0.94f, 0.012f, 0.004f, 0.002f, kGroove);
    for (int i = 0; i < 12; ++i) b.top(-1.22f + 0.08f * static_cast<float>(i), 0.135f, 0.36f, 0.012f, 0.24f, 0.004f, 0.002f, kGroove);
    b.loft(0.5f, 0.16f, 0.2f, 0.1f, 0.02f, {{0.36f, 0}, {0.385f, 0.006f}}, kButton);
    b.loft(0.82f, 0.16f, 0.2f, 0.1f, 0.02f, {{0.36f, 0}, {0.385f, 0.006f}}, kButton);
    b.front(0.3f, 0.08f, 0.63f, 0.66f, 0.022f, 0.006f, 0.002f, kHole);
    b.top(L::kSmsLed[0], L::kSmsLed[2], 0.36f, 0.06f, 0.06f, 0.03f, 0.002f, kBlackSoft);
}

void gameGear(Builder& b) {
    using L = ConsoleLayout;
    // 21 x 11.3 x 3.8 cm, rounded at both ends where the hands hold it.
    b.frame(0, 0.425f, 0, true);
    b.loft(0, 0, 1.56f, 0.84f, 0.3f, {{-0.14f, 0.035f}, {-0.12f, 0}, {0.11f, 0}, {0.14f, 0.035f}}, kBlack, true, 12);
    b.frame(0, 0, 0, false);
    const float z = 0.14f;
    // The screen's frame (the screen itself is drawn apart, to light up), the pad in its
    // round hollow, buttons 1 and 2, Start and the speaker's slits.
    b.front(0, L::kGgScreenY, z, 0.72f, 0.56f, 0.05f, 0.003f, kHole);
    b.front(0, L::kGgScreenY - 0.34f, z, 0.3f, 0.025f, 0.008f, 0.003f, kGgBlue);
    b.front(-0.56f, 0.42f, z, 0.3f, 0.3f, 0.15f, 0.002f, kHole);
    b.front(-0.56f, 0.42f, z, 0.22f, 0.075f, 0.015f, 0.022f, kBlackSoft);
    b.front(-0.56f, 0.42f, z, 0.075f, 0.22f, 0.015f, 0.022f, kBlackSoft);
    b.front(0.5f, 0.37f, z, 0.12f, 0.12f, 0.06f, 0.022f, kGgBlue);
    b.front(0.66f, 0.45f, z, 0.12f, 0.12f, 0.06f, 0.022f, kGgBlue);
    b.front(0.56f, 0.66f, z, 0.08f, 0.05f, 0.02f, 0.012f, kGgStart);
    for (int i = 0; i < 4; ++i) b.front(0.58f, 0.13f + 0.035f * static_cast<float>(i), z, 0.22f, 0.012f, 0.005f, 0.002f, kHole);
    b.front(L::kGgLed[0], L::kGgLed[1], z, 0.05f, 0.05f, 0.025f, 0.002f, kBlackSoft);
    // The slot along the top, and the power switch at its left.
    slot(b, 0, L::kGgSlotZ, L::kGgSlotY - 0.006f, 0.52f, 0.09f);
    b.top(-0.52f, 0.0f, L::kGgSlotY - 0.006f, 0.16f, 0.06f, 0.02f, 0.012f, kButton);
}

constexpr Colour kBoard{0.06f, 0.24f, 0.12f};
constexpr Colour kContact{0.8f, 0.62f, 0.24f};
constexpr Colour kShell{0.06f, 0.06f, 0.068f};

float mm(float v) { return v / 135.0f; }

// The board's edge at the bottom of a cartridge, inside the shell's opening, with its
// row of gold contacts on both faces (`pitch` mm apart).
void contacts(Builder& b, float halfW, float bottom, int count, float pitch, float board) {
    b.slab(mm(-halfW - 1.5f), bottom - mm(0.2f), mm(halfW + 1.5f), bottom + mm(2.5f), mm(-board / 2 - 2.2f), mm(board / 2 + 2.2f), kHole);
    b.slab(mm(-halfW), bottom + mm(0.4f), mm(halfW), bottom + mm(6), mm(-board / 2), mm(board / 2), kBoard);
    const float first = -pitch * static_cast<float>(count - 1) / 2;
    for (int i = 0; i < count; ++i) {
        const float x = first + pitch * static_cast<float>(i);
        for (float side : {1.0f, -1.0f})
            b.slab(mm(x - pitch * 0.32f), bottom + mm(0.4f), mm(x + pitch * 0.32f), bottom + mm(5.2f),
                   side > 0 ? mm(board / 2) : mm(-board / 2 - 0.05f), side > 0 ? mm(board / 2 + 0.05f) : mm(-board / 2), kContact);
    }
}

// Ribs across the shell to hold it by, on its front (`side` 1) or back (-1).
void ribs(Builder& b, float halfW, float y0, float y1, int count, float t, float side) {
    for (int i = 0; i < count; ++i) {
        const float y = y0 + (y1 - y0) * static_cast<float>(i) / static_cast<float>(std::max(1, count - 1));
        const float z = side * t / 2;
        b.slab(mm(-halfW), mm(y - 0.6f), mm(halfW), mm(y + 0.6f), std::min(z, z + side * mm(0.7f)), std::max(z, z + side * mm(0.7f)), kGroove);
    }
}

// The groove around the label's recess.
void recess(Builder& b, const CartridgeShape& c) {
    const float z = c.d / 2, e = mm(1.2f);
    const float x0 = -c.labelW / 2 - e, x1 = c.labelW / 2 + e, y0 = c.labelY - c.labelH / 2 - e, y1 = c.labelY + c.labelH / 2 + e;
    b.slab(x0, y0, x1, y1, z, z + mm(0.2f), kGroove);
}

}  // namespace

CartridgeShape cartridgeShape(ConsoleSystem s) {
    switch (s) {
        // 32X: 72 x 111 x 16 mm, upright.
        case ConsoleSystem::Mega32X: return {mm(72), mm(111), mm(16), mm(58), mm(66), mm(10), mm(36), mm(76)};
        // Master System: 108 x 69 x 16 mm, wider than tall.
        case ConsoleSystem::MasterSystem: return {mm(108), mm(69), mm(16), mm(92), mm(40), mm(-4), mm(28), mm(112)};
        // Game Gear: 66 x 68 x 10 mm; the lower half goes into the console.
        case ConsoleSystem::GameGear: return {mm(66), mm(68), mm(10), mm(54), mm(36), mm(12), mm(36), mm(70)};
        // Mega Drive (the European and American mould): 108 wide at the top, 95 where it
        // goes into the slot, 70 tall, 17 thick.
        default: return {mm(108), mm(70), mm(17), mm(82), mm(52), mm(-2), mm(30), mm(100)};
    }
}

void buildCartridgeModel(ConsoleSystem s, std::vector<float>& out) {
    Builder b(out);
    const CartridgeShape c = cartridgeShape(s);
    const float t = c.d / 2;
    switch (s) {
        case ConsoleSystem::Mega32X:
            // Narrower where it goes in, chamfered at the top.
            b.prismZ(-t, t, {{mm(-33), mm(-55.5f)}, {mm(33), mm(-55.5f)}, {mm(33), mm(-20)}, {mm(-33), mm(-20)}}, kShell);
            b.prismZ(-t, t, {{mm(-33), mm(-20)}, {mm(33), mm(-20)}, {mm(36), mm(-17)}, {mm(36), mm(49)}, {mm(30), mm(55.5f)},
                             {mm(-30), mm(55.5f)}, {mm(-36), mm(49)}, {mm(-36), mm(-17)}}, kShell);
            ribs(b, 28, 46, 53, 4, c.d, 1);
            ribs(b, 30, 30, 52, 8, c.d, -1);
            contacts(b, 30, mm(-55.5f), 30, 2.0f, 1.6f);
            break;
        case ConsoleSystem::MasterSystem:
            b.prismZ(-t, t, {{mm(-52), mm(-34.5f)}, {mm(52), mm(-34.5f)}, {mm(54), mm(-32.5f)}, {mm(54), mm(33)}, {mm(52.5f), mm(34.5f)},
                             {mm(-52.5f), mm(34.5f)}, {mm(-54), mm(33)}, {mm(-54), mm(-32.5f)}}, kShell);
            ribs(b, 46, 20, 31, 5, c.d, 1);
            ribs(b, 48, 18, 31, 6, c.d, -1);
            contacts(b, 34, mm(-34.5f), 25, 2.54f, 1.6f);
            break;
        case ConsoleSystem::GameGear:
            b.prismZ(-t, t, {{mm(-31), mm(-34)}, {mm(31), mm(-34)}, {mm(33), mm(-32)}, {mm(33), mm(30)}, {mm(29), mm(34)},
                             {mm(-29), mm(34)}, {mm(-33), mm(30)}, {mm(-33), mm(-32)}}, kShell);
            ribs(b, 26, -24, -12, 4, c.d, -1);
            contacts(b, 24, mm(-34), 23, 2.0f, 1.2f);
            break;
        default:
            // The part that goes into the slot, 95 mm wide, then a chamfer out to 108.
            b.prismZ(-t, t, {{mm(-47.5f), mm(-35)}, {mm(47.5f), mm(-35)}, {mm(47.5f), mm(8)}, {mm(-47.5f), mm(8)}}, kShell);
            b.prismZ(-t, t, {{mm(-47.5f), mm(8)}, {mm(47.5f), mm(8)}, {mm(54), mm(12)}, {mm(54), mm(33)}, {mm(52), mm(35)},
                             {mm(-52), mm(35)}, {mm(-54), mm(33)}, {mm(-54), mm(12)}}, kShell);
            ribs(b, 46, 27, 33, 3, c.d, 1);
            ribs(b, 48, 16, 33, 7, c.d, -1);
            contacts(b, 41, mm(-35), 32, 2.54f, 1.6f);
            break;
    }
    recess(b, c);
    // The screw at the back.
    b.slab(mm(-2.5f), mm(-6), mm(2.5f), mm(-1), -t - mm(0.3f), -t, kHole);
}

void buildConsoleModel(ConsoleModel model, std::vector<float>& out) {
    Builder b(out);
    switch (model) {
        case ConsoleModel::MegaDrive: megaDrive(b); break;
        case ConsoleModel::Mega32X: mega32x(b); break;
        case ConsoleModel::MegaCD: megaCd(b); break;
        case ConsoleModel::MegaCDTray: megaCdTray(b); break;
        case ConsoleModel::MasterSystem: masterSystem(b); break;
        case ConsoleModel::GameGear: gameGear(b); break;
        default: break;
    }
}

}  // namespace plumsg
