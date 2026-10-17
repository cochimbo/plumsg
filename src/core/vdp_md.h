#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "video_frame_md.h"

namespace plumsg {

// Mega Drive VDP (Yamaha YM7101): 64 KB VRAM, 64-colour CRAM, 40-entry VSRAM and 24
// registers. Draws whole lines: planes A and B with full/cell/line horizontal scroll
// and full/2-cell vertical scroll, the window, sprites with the per-line limits and
// masking, priorities, shadow/highlight, H32/H40, V28/V30 and interlace mode 2.
// DMA (68000 to VRAM/CRAM/VSRAM, fill and copy) runs at once; the time a 68000 DMA
// would take is returned so the CPU can be stalled for it.
//
// Every line it renders goes into a VideoFrameMd twice: the final mix and the three
// layers apart, which is what the pseudo-3D view needs.
class VdpMd {
public:
    static constexpr int kMasterPerLine = 3420;

    VdpMd();

    // Reads a word from the 68000 bus, for DMA.
    void setDmaReader(std::function<uint16_t(uint32_t)> reader) { dmaRead_ = std::move(reader); }
    void setPal(bool pal) { pal_ = pal; }
    bool pal() const { return pal_; }

    void reset();

    // CPU ports. Writing the control port can start a 68000 DMA; the return value is how
    // many master clocks the 68000 is held off the bus for it (0 otherwise).
    int writeControl(uint16_t value);
    int writeData(uint16_t value);
    uint16_t readData();
    uint16_t readStatus();
    // H/V counter for the current position: `lineClock` master clocks into the line.
    uint16_t readHV(int lineClock) const;

    // Called by the scheduler.
    int linesPerFrame() const { return pal_ ? 313 : 262; }
    int activeLines() const { return (pal_ && (regs_[1] & 0x08)) ? 240 : 224; }
    void startLine(int line);   // updates the V counter, VBlank and the VINT
    void renderLine(int line);  // draws an active line with the registers as they are now
    void endLine(int line);     // HINT counter (at the start of horizontal blanking)
    void endFrame();

    // Interrupts: level 6 (VINT) and level 4 (HINT), already masked by their enables.
    int irqLevel() const;
    void acknowledge(int level);
    bool z80Irq() const { return z80Irq_; }

    const VideoFrameMd& frame() const { return *frame_; }
    bool displayEnabled() const { return (regs_[1] & 0x40) != 0; }
    bool h40() const { return (regs_[12] & 0x01) != 0; }
    uint8_t reg(int r) const { return regs_[r]; }
    const std::array<uint8_t, 0x10000>& vram() const { return vram_; }
    const std::array<uint16_t, 64>& cram() const { return cram_; }

    template <class Ar>
    void state(Ar& ar) {
        ar(vram_, cram_, vsram_, regs_, pendingControl_, code_, addr_, fillPending_, readBuffer_, vcounter_,
           hintCounter_, vintPending_, hintPending_, z80Irq_, spriteOverflow_, spriteCollision_, oddFrame_,
           inVblank_, dmaBusyLines_, pal_, frameNumber_);
        if constexpr (Ar::kLoading) cacheColors();
    }

private:
    void writeRegister(int reg, uint8_t value);
    void writeWord(uint16_t value);  // to the memory selected by code_/addr_, then auto-increment
    int runDma();                    // starts the DMA selected by the registers
    void dmaFill(uint16_t value);
    uint32_t dmaLength() const;
    void cacheColors();
    uint32_t color(int index, MdShade shade) const;

    // Line rendering helpers.
    struct Cell {
        uint8_t color;    // palette * 16 + index
        bool priority;
        uint16_t tile;
    };
    void drawPlane(int line, bool planeA, std::array<Cell, 320>& out, std::array<bool, 320>& isWindow);
    void drawSprites(int line, std::array<Cell, 320>& out, std::array<uint8_t, 320>& number);

    std::array<uint8_t, 0x10000> vram_{};
    std::array<uint16_t, 64> cram_{};
    std::array<uint16_t, 40> vsram_{};
    std::array<uint8_t, 24> regs_{};

    bool pendingControl_ = false;  // first half of a two-word command written
    uint8_t code_ = 0;
    uint32_t addr_ = 0;
    bool fillPending_ = false;     // DMA fill armed: waits for a data port write
    uint16_t readBuffer_ = 0;

    int vcounter_ = 0;
    int hintCounter_ = 0;
    bool vintPending_ = false;
    bool hintPending_ = false;
    bool z80Irq_ = false;
    bool spriteOverflow_ = false;
    bool spriteCollision_ = false;
    bool oddFrame_ = false;
    bool inVblank_ = false;
    int dmaBusyLines_ = 0;  // fill/copy still "running", for the status bit
    bool pal_ = false;
    uint64_t frameNumber_ = 0;

    std::array<uint32_t, 64 * 3> colors_{};  // CRAM as RGBA, normal/shadow/highlight
    std::unique_ptr<VideoFrameMd> frame_;
    std::function<uint16_t(uint32_t)> dmaRead_;
};

}  // namespace plumsg
