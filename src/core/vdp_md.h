#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "video_frame_md.h"

namespace plumsg {

// Mega Drive VDP (Yamaha YM7101): 64 KB VRAM, 64-colour CRAM, 40-entry VSRAM and 24
// registers. Draws whole lines: planes A and B with full/cell/line horizontal scroll
// and full/2-cell vertical scroll, the window, sprites with the per-line limits (or
// without them, as an option) and masking, priorities, shadow/highlight, H32/H40,
// V28/V30 and interlace mode 2.
// Writes go through the console's 4-word FIFO and reach memory in the VDP's access
// slots (18 a line in H40, 16 in H32 while drawing, where the hardware has them; about 205/167 in blanking), a VRAM
// word taking two of them. DMA runs through the same slots: a 68000 DMA feeds the FIFO
// and holds the CPU until its last word is in; fill and copy run on their own while the
// CPU goes on. The port calls that take a master clock run it in time; the ones without
// do everything at once (tests).
//
// Every line it renders goes into a VideoFrameMd twice: the final mix and the three
// layers apart, which is what the pseudo-3D view needs.
class VdpMd {
public:
    static constexpr int kMasterPerLine = 3420;
    // Where the active part of a line ends and horizontal blanking starts, in master
    // clocks from the start of the line. Lines are drawn and HINT fires here.
    static constexpr int kActiveClocks = 2560;
    // The access slots of a line: how many fall in its picture part (before
    // kActiveClocks) and how many in its horizontal blanking, and where (master clocks
    // from the start of the line; none: spread evenly over the part).
    struct SlotLayout {
        int picture, blank;
        const uint16_t* pictureAt;
        const uint16_t* blankAt;
    };
    // Where the V counter moves on to the line's number (before, it still shows the
    // previous one), and VBlank starts and ends. testpico's HBlank tests, which read the
    // status right after the V counter changes, pin it to 2564-2573 in both H32 and H40.
    static constexpr int kVcountClocks = 2568;

    VdpMd();

    // Reads a word from the 68000 bus, for DMA.
    void setDmaReader(std::function<uint16_t(uint32_t)> reader) { dmaRead_ = std::move(reader); }
    void setPal(bool pal) { pal_ = pal; }
    bool pal() const { return pal_; }
    // 68000 DMA from the first 64 KB (a cartridge chip's RAM) done at once, the CPU not held.
    void setInstantDma(bool on) { instantDma_ = on; }

    void reset();

    // CPU ports at master clock `clock` (counted as the scheduler counts it). The return
    // value is how many master clocks the CPU waits: for a 68000 DMA to take its last
    // word, for room in a full FIFO, or for the FIFO to empty before a read.
    int writeControl(uint16_t value, uint64_t clock);
    int writeData(uint16_t value, uint64_t clock);
    uint16_t readData(uint64_t clock, int* wait = nullptr);
    // The same with every write and DMA finished at once.
    int writeControl(uint16_t value);
    int writeData(uint16_t value);
    uint16_t readData();
    // The status register at `clock`, `lineClock` master clocks into the current line.
    // Bits 15-10 are not driven by the VDP: they read as 0 here.
    uint16_t readStatus(int lineClock, uint64_t clock);
    uint16_t readStatus(int lineClock);
    // H/V counter for the current position: `lineClock` master clocks into the line.
    uint16_t readHV(int lineClock) const;
    int counterLine(int lineClock) const;  // the line the V counter is on

    // Called by the scheduler.
    int linesPerFrame() const { return pal_ ? 313 : 262; }
    int activeLines() const { return (pal_ && (regs_[1] & 0x08)) ? 240 : 224; }
    void startLine(int line, uint64_t clock);  // updates the V counter and VBlank
    void startLine(int line) { startLine(line, ~uint64_t{0}); }
    // Runs the access slots up to `clock`: what the FIFO and a DMA write by then is in memory.
    void sync(uint64_t clock);
    // The VINT comes this many master clocks after VBlank starts (at kVcountClocks of the
    // first line below the picture): games that wait for the VBlank flag with interrupts
    // on see it before their handler runs (Snatcher).
    static constexpr int kVintDelay = 788;
    void raiseVint();
    void renderLine(int line);  // draws an active line with the registers as they are now
    void endLine(int line);     // HINT counter (at the start of horizontal blanking)
    void endFrame();

    // Interrupts: level 6 (VINT) and level 4 (HINT), already masked by their enables.
    int irqLevel() const;
    void acknowledge(int level);
    bool z80Irq() const { return z80Irq_; }

    const VideoFrameMd& frame() const { return *frame_; }
    VideoFrameMd& frame() { return *frame_; }  // for the 32X, which draws over it
    bool displayEnabled() const { return (regs_[1] & 0x40) != 0; }
    bool h40() const { return (regs_[12] & 0x01) != 0; }
    uint8_t reg(int r) const { return regs_[r]; }
    const std::array<uint8_t, 0x10000>& vram() const { return vram_; }
    const std::array<uint16_t, 64>& cram() const { return cram_; }
    // Draws every sprite on a line instead of stopping at the console's 16/20 sprites and
    // 256/320 pixels per line, which is what makes games flicker. The status bits and
    // everything the game can read stay as on the console, so it is not part of the state.
    void setUnlimitedSprites(bool on) { unlimitedSprites_ = on; }
    bool unlimitedSprites() const { return unlimitedSprites_; }
    // Widescreen: draws `left` and `right` more columns (counted at 320 pixels a line; H32
    // gets as many as cover the same part of the screen) that the console never shows,
    // taken from the planes and sprites past its edges. 0 and 0 turn it off. Only the
    // picture changes, as with the sprite limit, so it is not part of the state either.
    // It can change at any frame.
    void setWidescreen(int left, int right) {
        wideLeft_ = std::clamp(left, 0, VideoFrameMd::kMaxWideSide);
        wideRight_ = std::clamp(right, 0, VideoFrameMd::kMaxWideSide);
    }
    // The columns on each side, at 320 pixels a line, that make a W:H picture (16:9: 53).
    static int wideColumnsFor(int aspectW, int aspectH);

    template <class Ar>
    void state(Ar& ar) {
        int unusedBusyLines = 0;  // kept so older states still load
        bool unusedVblank = false;
        ar(vram_, cram_, vsram_, regs_, pendingControl_, code_, addr_, fillPending_, readBuffer_, vcounter_,
           hintCounter_, vintPending_, hintPending_, z80Irq_, spriteOverflow_, spriteCollision_, oddFrame_,
           unusedVblank, unusedBusyLines, pal_, frameNumber_);
        if constexpr (Ar::kLoading) {
            cacheColors();
            // A state without the FIFO's own block: nothing is waiting, and the slots
            // line up again with the next line.
            clearFifo();
        }
    }
    // The FIFO, the DMA under way and the slot clock, in a block of their own.
    template <class Ar>
    void fifoState(Ar& ar) {
        ar(fifoData_, fifoNext_, queueCount_, halfDone_, dmaMode_, dmaLeft_, dmaSource_, fillValue_, copyRead_,
           copyByte_, slotLine_, slotLineStart_, syncedTo_, hvLatch_);
        for (FifoEntry& e : queue_) ar(e.addr, e.value, e.code, e.startsFill);
    }

private:
    // A write waiting in the FIFO for its slot.
    struct FifoEntry {
        uint32_t addr = 0;
        uint16_t value = 0;
        uint8_t code = 0;
        bool startsFill = false;  // the data port write a DMA fill was waiting for
    };
    enum DmaMode : uint8_t { kDmaNone, kDmaTransfer, kDmaFill, kDmaCopy };
    static constexpr uint64_t kUntimed = ~uint64_t{0};

    void writeRegister(int reg, uint8_t value);
    // Stores a word in VRAM, CRAM or VSRAM as the FIFO hands it over.
    void storeWord(uint8_t code, uint32_t addr, uint16_t value);
    void push(uint16_t value);  // into the FIFO, at code_/addr_, then auto-increment
    int startDma(uint64_t clock);
    uint32_t dmaLength() const;
    void refill();  // a 68000 DMA puts its next words in the FIFO while there is room
    void finishDma();
    // One access slot: the oldest write in the FIFO, or a step of a fill or copy.
    void slot();
    bool busy() const { return queueCount_ > 0 || dmaMode_ != kDmaNone; }
    SlotLayout slotLayout(int line) const;
    // The time of the n-th access slot after `clock` (n >= 1), counting lines on from the
    // one the slots are at now, with the registers as they are.
    uint64_t slotAfter(uint64_t clock, uint64_t n) const;
    uint64_t slotsToDrain() const;  // slots until the FIFO is empty and the DMA done
    void drain();                   // finishes everything now
    void clearFifo();
    int entrySlots(const FifoEntry& e) const { return (e.code & 0x0F) == 1 ? 2 : 1; }
    void cacheColors();
    uint32_t color(int index, MdShade shade) const;

    // Line rendering helpers.
    struct Cell {
        uint8_t color;    // palette * 16 + index
        bool priority;
        uint16_t tile;
    };
    // They fill `out[0, left + width + right)` for screen columns -left .. width + right.
    using LineCells = std::array<Cell, VideoFrameMd::kMaxWidth>;
    void drawPlane(int line, bool planeA, int left, int right, LineCells& out,
                   std::array<bool, VideoFrameMd::kMaxWidth>& isWindow);
    void drawSprites(int line, int left, int right, LineCells& out, std::array<uint8_t, VideoFrameMd::kMaxWidth>& number);

    std::array<uint8_t, 0x10000> vram_{};
    std::array<uint16_t, 64> cram_{};
    std::array<uint16_t, 40> vsram_{};
    std::array<uint8_t, 24> regs_{};

    bool pendingControl_ = false;  // first half of a two-word command written
    uint8_t code_ = 0;
    uint32_t addr_ = 0;
    bool fillPending_ = false;     // DMA fill armed: waits for a data port write
    uint16_t readBuffer_ = 0;
    uint16_t hvLatch_ = 0;         // the HV counter while the latch (register 0 bit 1) is on

    // The FIFO's four words as last written: reads from CRAM, VSRAM and the 8-bit VRAM
    // target take the bits those memories lack from the oldest one, fifoData_[fifoNext_].
    std::array<uint16_t, 4> fifoData_{};
    uint8_t fifoNext_ = 0;
    // The writes still waiting, oldest first; a fifth one is the CPU's, held until there
    // is room.
    std::array<FifoEntry, 5> queue_{};
    uint8_t queueCount_ = 0;
    bool halfDone_ = false;  // the oldest is a VRAM word with one of its two slots spent
    uint8_t dmaMode_ = kDmaNone;
    uint32_t dmaLeft_ = 0;     // words (transfer), bytes or words (fill), bytes (copy)
    uint32_t dmaSource_ = 0;   // transfer: the 68000 address it reads; copy: the VRAM one
    uint16_t fillValue_ = 0;
    bool copyRead_ = false;    // copy: the byte is read, its write comes next slot
    uint8_t copyByte_ = 0;
    // The slot clock: the line the slots are counted in, where it starts, and how far
    // they have run.
    int slotLine_ = 0;
    uint64_t slotLineStart_ = 0;
    uint64_t syncedTo_ = 0;

    int vcounter_ = 0;
    int hintCounter_ = 0;
    bool vintPending_ = false;
    bool hintPending_ = false;
    bool z80Irq_ = false;
    bool spriteOverflow_ = false;
    bool spriteCollision_ = false;
    bool dotOverflow_ = false;  // the last line drawn ran out of sprite pixels (not part of the state)
    bool oddFrame_ = false;
    bool pal_ = false;
    bool instantDma_ = false;
    uint64_t frameNumber_ = 0;
    bool unlimitedSprites_ = false;
    int wideLeft_ = 0, wideRight_ = 0;

    std::array<uint32_t, 64 * 3> colors_{};  // CRAM as RGBA, normal/shadow/highlight
    // With the display off the screen shows the backdrop colour as it is at each pixel:
    // the colours written to CRAM during the line, with when (for "direct colour" DMA
    // pictures). Not part of the state; the slot being run is `slotClock_`.
    struct CramWrite {
        uint64_t clock;
        uint8_t index;
        uint32_t before, after;
    };
    std::array<CramWrite, 256> cramWrites_{};
    int cramWriteCount_ = 0;
    uint64_t slotClock_ = kUntimed;
    std::unique_ptr<VideoFrameMd> frame_;
    std::function<uint16_t(uint32_t)> dmaRead_;
};

}  // namespace plumsg
