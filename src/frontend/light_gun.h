#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "../core/io.h"

namespace plumsg {

// The light guns' crosshairs on screen: one a gun, moved with the mouse (only the first)
// or with that player's left stick or d-pad, and what each gun shows the console. The
// aim is kept as a fraction of the console's own picture (0..1 across and down), so it
// stays put when the window or the picture's size changes. main() hands it the mouse,
// the pads and where the picture is; the UI draws the crosshairs.
class LightGunAim {
public:
    // Where the console's picture is, in window coordinates (origin at the top left).
    void setPicture(float x, float y, float width, float height) {
        px_ = x;
        py_ = y;
        pw_ = std::max(width, 1.0f);
        ph_ = std::max(height, 1.0f);
    }
    // The mouse moved to (x, y): it takes the first gun there, also off the picture,
    // where the gun sees no light.
    void mouseMoved(float x, float y) {
        aimX_[0] = (x - px_) / pw_;
        aimY_[0] = (y - py_) / ph_;
    }
    // The mouse's buttons, for the first gun: left the trigger, right the trigger off the
    // screen (to reload), middle Start.
    void mouseButtons(bool left, bool right, bool middle) {
        mouseTrigger_ = left;
        mouseReload_ = right;
        mouseStart_ = middle;
    }
    // Moves a gun's crosshair for `seconds`: the stick (each axis -1..1, 0 when at rest)
    // goes across the picture in about a second when pushed all the way, finer when
    // pushed a little; without it the d-pad goes at a steady pace. It stays on the
    // picture.
    void move(int gun, const PadState& pad, float stickX, float stickY, float seconds) {
        float dx = stickX * std::abs(stickX), dy = stickY * std::abs(stickY);
        if (stickX == 0 && stickY == 0) {
            dx = (pad.right ? kPadSpeed : 0.0f) - (pad.left ? kPadSpeed : 0.0f);
            dy = (pad.down ? kPadSpeed : 0.0f) - (pad.up ? kPadSpeed : 0.0f);
        }
        if (dx == 0 && dy == 0) return;
        auto& x = aimX_[static_cast<size_t>(gun & 1)];
        auto& y = aimY_[static_cast<size_t>(gun & 1)];
        // Up and down as fast on screen as across: the picture is 4:3.
        x = std::clamp(x + dx * seconds, 0.0f, kLast);
        y = std::clamp(y + dy * seconds * 4.0f / 3.0f, 0.0f, kLast);
    }
    // What a gun shows the console, on a `width` x `height` picture, with its player's
    // pad: A the trigger, C the trigger off the screen, Start; B and X the Menacer's A
    // and B. The first gun adds the mouse's buttons.
    GunState state(int gun, const PadState& pad, int width, int height) const {
        GunState s;
        const size_t g = static_cast<size_t>(gun & 1);
        const bool mouse = g == 0;
        const bool reload = pad.c || (mouse && mouseReload_);
        s.trigger = pad.a || reload || (mouse && mouseTrigger_);
        s.start = pad.start || (mouse && mouseStart_);
        s.a = pad.b;
        s.b = pad.x;
        if (!reload && aimX_[g] >= 0 && aimY_[g] >= 0 && aimX_[g] < 1 && aimY_[g] < 1) {
            s.x = static_cast<int>(aimX_[g] * static_cast<float>(width));
            s.y = static_cast<int>(aimY_[g] * static_cast<float>(height));
        }
        return s;
    }
    // Where a crosshair is in the window, and whether it is on the picture.
    bool crosshair(int gun, float* x, float* y) const {
        const size_t g = static_cast<size_t>(gun & 1);
        *x = px_ + aimX_[g] * pw_;
        *y = py_ + aimY_[g] * ph_;
        return aimX_[g] >= 0 && aimY_[g] >= 0 && aimX_[g] < 1 && aimY_[g] < 1;
    }
    // Whether the window point (x, y) is on the picture.
    bool over(float x, float y) const { return x >= px_ && y >= py_ && x < px_ + pw_ && y < py_ + ph_; }

private:
    static constexpr float kPadSpeed = 0.6f;  // pictures a second
    static constexpr float kLast = 0.999f;    // the last pixel, not past it
    std::array<float, 2> aimX_{0.5f, 0.5f}, aimY_{0.5f, 0.5f};
    float px_ = 0, py_ = 0, pw_ = 1, ph_ = 1;
    bool mouseTrigger_ = false, mouseReload_ = false, mouseStart_ = false;
};

}  // namespace plumsg
