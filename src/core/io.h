#pragma once

#include <array>
#include <cstdint>

namespace plumsg {

// Buttons of one Mega Drive pad, true while held.
struct PadState {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, c = false, start = false;
    bool x = false, y = false, z = false, mode = false;
};

// None: nothing plugged in. Some games look: ESPN Sunday Night NFL makes player 2 the
// computer when the second port is empty, and waits for pad 2 at the coin toss if not.
// Menacer and Justifier: light guns, which go in the second port (Io only reads them there).
// LightPhaser and Paddle: the Master System's light gun and its paddle (HPD-200), in
// either port; only Master System games see them, and a Mega Drive game a 3-button pad.
enum class PadType : uint8_t {
    ThreeButton = 0, SixButton = 1, None = 2, Menacer = 3, Justifier = 4, LightPhaser = 5, Paddle = 6
};
constexpr int kPadTypes = 7;
inline bool isLightGun(PadType t) { return t == PadType::Menacer || t == PadType::Justifier; }
inline bool isMasterSystemController(PadType t) { return t == PadType::LightPhaser || t == PadType::Paddle; }

// The Master System paddle: its knob, 0 (all to the left) to 255, and its button.
struct PaddleState {
    uint8_t position = 128;
    bool button = false;
};

// Where a light gun points and what is held on it. x and y are pixels of the console's
// picture (0..255 or 0..319 across, 0..223 or 0..239 down); anywhere else, as when the
// trigger is pulled off the screen to reload, the gun sees no light. The Justifier has
// only the trigger and Start; the Menacer also has A and B.
struct GunState {
    int x = -1, y = -1;
    bool trigger = false, a = false, b = false, start = false;
    bool onScreen(int width, int height) const { return x >= 0 && y >= 0 && x < width && y < height; }
};

// The I/O chip: version register and the two controller ports (the third, the
// modem/extension port, reads as nothing connected), and each port's serial registers,
// which keep what is written though nothing is ever sent or received. Pads are read through the TH line:
// a 3-button pad multiplexes its buttons on TH; a 6-button pad also counts TH pulses
// and shows X/Y/Z/Mode on the fourth one, until TH rests for about 1.5 ms. A TH pin
// switched from output to input does not rise at once: its pull-up takes a few
// microseconds to charge the line, and some games read the pad in that time. A light
// gun in the second port answers instead with its own buttons.
class Io {
public:
    void reset();
    void setVersion(uint8_t v) { version_ = v; }
    void setPad(int port, const PadState& s) { pads_[port & 1] = s; }
    void setPadType(int port, PadType t) { types_[port & 1] = t; }
    PadType padType(int port) const { return types_[port & 1]; }
    // Light gun 0, and the Justifier's second (pink) gun 1, in the second port.
    void setGun(int gun, const GunState& s) { guns_[gun & 1] = s; }
    // The gun the console would see the light of now, or null: one plugged in, enabled
    // (the Justifier picks one of its two with TH and TR) and with the second port's
    // TH interrupt on (control register bit 7), which is what latches the HV counter.
    const GunState* activeGun() const;

    // 0xA10001-0xA1001F, odd addresses. `now` is the time in master clocks, for the
    // 6-button pad's timeout.
    uint8_t read(uint32_t addr, uint64_t now);
    void write(uint32_t addr, uint8_t value, uint64_t now);

    template <class Ar>
    void state(Ar& ar) {
        ar(data_, ctrl_, version_, thCount_, lastTh_, thTime_, types_);
    }
    // When each port's TH was let go while low (kept apart: older states lack it).
    template <class Ar>
    void riseState(Ar& ar) {
        ar(thRelease_);
    }
    // The serial registers (kept apart too).
    template <class Ar>
    void serialState(Ar& ar) {
        ar(txData_, serialCtrl_);
    }

private:
    uint8_t readPort(int port, uint64_t now);
    uint8_t readGun(PadType type) const;
    bool thLevel(int port, uint64_t now) const;

    std::array<uint8_t, 3> data_{};
    std::array<uint8_t, 3> ctrl_{};
    uint8_t version_ = 0xA0;
    std::array<uint8_t, 2> thCount_{};   // TH falling edges seen (6-button protocol), 0..4
    std::array<bool, 2> lastTh_{true, true};
    std::array<uint64_t, 2> thTime_{};
    // The time TH became an input while driven low, or kNotRising.
    std::array<uint64_t, 2> thRelease_{kNotRising, kNotRising};
    static constexpr uint64_t kNotRising = ~uint64_t{0};
    std::array<PadType, 2> types_{PadType::SixButton, PadType::SixButton};
    std::array<PadState, 2> pads_{};
    std::array<GunState, 2> guns_{};
    std::array<uint8_t, 3> txData_{0xFF, 0xFF, 0xFF};
    std::array<uint8_t, 3> serialCtrl_{};  // bits 3-7; 0-2 are status (nothing received)
};

}  // namespace plumsg
