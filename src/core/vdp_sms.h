#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "video_frame_md.h"

namespace plumsg {

// Master System VDP (Sega 315-5124/5246, a TMS9918 with a "mode 4" of its own): 16 KB
// VRAM, 32 colours of 6 bits, 11 registers. Draws whole lines in mode 4: a 32x28 (or
// 32x32 in the 224 and 240 line modes) background of 8x8 tiles with horizontal scroll
// per line and vertical scroll per frame, the two scroll locks, the left column mask,
// tiles in front of sprites, and up to 64 sprites (8 per line) of 8x8 or 8x16, zoomed or
// not, with the overflow and collision flags.
//
// Without mode 4 it draws the TMS9918's own modes, as the first Master System does,
// for the few games that use them: Graphics I and II and Text, with the TMS9918's 32
// sprites of one colour (4 per line), in its 15 colours as the Master System shows them.
//
// As the Game Gear's VDP (setGameGear) its colours have 12 bits, written to CRAM a
// byte at a time, and only the middle 160x144 of the picture is seen: that is what
// goes into the frame.
//
// It draws into a VideoFrameMd, the same frame the Mega Drive VDP fills, so the
// renderers, screenshots and recordings work unchanged: the background goes to plane B,
// its tiles with priority (where they are not colour 0) also to plane A, and the sprites
// to the sprite layer. Colours are 0..31 (background palette 0..15, sprite palette 16..31).
class VdpSms {
public:
    explicit VdpSms(VideoFrameMd& frame) : frame_(frame) { reset(); }

    void reset();
    void setPal(bool pal) { pal_ = pal; }
    void setGameGear(bool on) {
        gameGear_ = on;
        cacheColors();
    }
    bool gameGear() const { return gameGear_; }
    // The Game Gear's screen inside the 256-pixel lines.
    static constexpr int kGgWidth = 160, kGgHeight = 144, kGgLeft = 48;
    bool pal() const { return pal_; }

    // CPU ports: 0xBE data, 0xBF control, 0x7E V counter, 0x7F H counter.
    uint8_t readData();
    void writeData(uint8_t value);
    uint8_t readStatus();  // clears the flags and the interrupt
    void writeControl(uint8_t value);
    uint8_t vcounter() const;
    // The H counter at `cycle` Z80 cycles into the line (it counts pixels / 2).
    uint8_t hcounter(int cycle) const;

    // Called by the scheduler at the start of every line: draws it if it is an active
    // one (and `draw`), runs the line counter and raises the frame interrupt.
    int linesPerFrame() const { return pal_ ? 313 : 262; }
    int activeLines() const;
    void startLine(int line, bool draw);
    void endFrame();

    // The Z80's INT line: a pending frame or line interrupt whose enable is set.
    bool irq() const;

    // Draws every sprite on a line instead of stopping at 8 (the status bits stay as on
    // the console). Not part of the state.
    void setUnlimitedSprites(bool on) { unlimitedSprites_ = on; }

    uint8_t reg(int r) const { return regs_[static_cast<size_t>(r & 15)]; }
    const std::array<uint8_t, 0x4000>& vram() const { return vram_; }
    const std::array<uint8_t, 32>& cram() const { return cram_; }
    uint32_t color(int index) const { return colors_[static_cast<size_t>(index & 31)]; }

    template <class Ar>
    void state(Ar& ar) {
        ar(vram_, cram_, regs_, pendingControl_, code_, addr_, latch_, readBuffer_, line_, lineCounter_, frameIrq_,
           lineIrq_, spriteOverflow_, spriteCollision_, vscrollLatch_, pal_, frameNumber_);
        if constexpr (Ar::kLoading) {
            cramHigh_.fill(0);  // until gameGearState() says otherwise
            cramLatch_ = 0;
            cacheColors();
        }
    }
    // What the Game Gear added, kept apart so that older states still load.
    template <class Ar>
    void gameGearState(Ar& ar) {
        ar(cramHigh_, cramLatch_);
        if constexpr (Ar::kLoading) cacheColors();
    }

private:
    void cacheColors();
    void renderLine(int line);
    void renderLineTms(int line);
    void cropGameGear(int line);
    bool mode4() const { return (regs_[0] & 0x04) != 0; }
    uint16_t nameTableBase() const;

    std::array<uint8_t, 0x4000> vram_{};
    std::array<uint8_t, 32> cram_{};      // on the Game Gear, the low byte of each colour
    std::array<uint8_t, 32> cramHigh_{};  // the Game Gear's high bytes (blue)
    uint8_t cramLatch_ = 0;               // the Game Gear's low byte, until the high one comes
    bool gameGear_ = false;
    std::array<uint8_t, 16> regs_{};
    bool pendingControl_ = false;  // first byte of a command written
    uint8_t code_ = 0;
    uint16_t addr_ = 0;
    uint8_t latch_ = 0;            // that first byte
    uint8_t readBuffer_ = 0;
    int line_ = 0;
    int lineCounter_ = 0;
    bool frameIrq_ = false;
    bool lineIrq_ = false;
    bool spriteOverflow_ = false;
    bool spriteCollision_ = false;
    uint8_t vscrollLatch_ = 0;     // register 9 as it was when the frame started
    bool pal_ = false;
    uint64_t frameNumber_ = 0;
    bool unlimitedSprites_ = false;

    std::array<uint32_t, 32> colors_{};
    VideoFrameMd& frame_;
};

}  // namespace plumsg
