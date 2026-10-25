#include "vdp_md.h"

#include <algorithm>

namespace plumsg {

namespace {

constexpr int kPlaneCells[4] = {32, 64, 32, 128};  // size code 2 is invalid; acts as 32

}  // namespace

VdpMd::VdpMd() : frame_(std::make_unique<VideoFrameMd>()) { reset(); }

void VdpMd::reset() {
    vram_.fill(0);
    cram_.fill(0);
    vsram_.fill(0);
    regs_.fill(0);
    pendingControl_ = false;
    code_ = 0;
    addr_ = 0;
    fillPending_ = false;
    readBuffer_ = 0;
    hvLatch_ = 0;
    vcounter_ = 0;
    hintCounter_ = 0;
    vintPending_ = hintPending_ = z80Irq_ = false;
    spriteOverflow_ = spriteCollision_ = false;
    oddFrame_ = false;
    fifoData_.fill(0);
    fifoNext_ = 0;
    clearFifo();
    slotLine_ = 0;
    slotLineStart_ = syncedTo_ = 0;
    cacheColors();
}

void VdpMd::cacheColors() {
    for (int i = 0; i < 64; ++i) {
        const uint16_t c = cram_[i];
        const int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
        // 4-bit output levels: normal 0,2..14; shadow 0..7; highlight 7..14.
        auto pack = [](int lr, int lg, int lb) {
            auto to8 = [](int l) { return static_cast<uint32_t>(l * 255 / 14); };
            return 0xFF000000u | (to8(lr) << 16) | (to8(lg) << 8) | to8(lb);
        };
        colors_[i] = pack(r * 2, g * 2, b * 2);
        colors_[64 + i] = pack(r, g, b);
        colors_[128 + i] = pack(7 + r, 7 + g, 7 + b);
    }
}

uint32_t VdpMd::color(int index, MdShade shade) const { return colors_[static_cast<int>(shade) * 64 + (index & 63)]; }

void VdpMd::writeRegister(int reg, uint8_t value) {
    // Without mode 5 (M5 in register 1) only registers 0-10 take writes.
    if (reg >= 24 || (reg > 10 && !(regs_[1] & 0x04))) return;
    regs_[reg] = value;
}

void VdpMd::clearFifo() {
    queueCount_ = 0;
    halfDone_ = false;
    dmaMode_ = kDmaNone;
    dmaLeft_ = 0;
    copyRead_ = false;
}

int VdpMd::writeControl(uint16_t value) { return writeControl(value, kUntimed); }
int VdpMd::writeData(uint16_t value) { return writeData(value, kUntimed); }
uint16_t VdpMd::readData() { return readData(kUntimed); }
uint16_t VdpMd::readStatus(int lineClock) { return readStatus(lineClock, kUntimed); }

int VdpMd::writeControl(uint16_t value, uint64_t clock) {
    sync(clock);
    if (!pendingControl_) {
        if ((value & 0xC000) == 0x8000) {
            const int reg = (value >> 8) & 0x1F;
            if (reg == 0 && (value & 0x02) && !(regs_[0] & 0x02)) {
                // Turning the HV counter latch on freezes the counter where it is.
                const int lineClock = clock == kUntimed ? 0 : static_cast<int>(clock - slotLineStart_);
                hvLatch_ = readHV(lineClock);
            }
            writeRegister(reg, static_cast<uint8_t>(value));
            // The VDP also takes it as a command's first word: CD1-CD0 become 10, which no
            // memory answers to, so data port writes after it go nowhere.
            code_ = static_cast<uint8_t>((code_ & 0x3C) | 0x02);
            return 0;
        }
        code_ = static_cast<uint8_t>((code_ & 0x3C) | (value >> 14));
        addr_ = (addr_ & 0x1C000) | (value & 0x3FFF);
        pendingControl_ = true;
        return 0;
    }
    pendingControl_ = false;
    code_ = static_cast<uint8_t>((code_ & 0x03) | ((value >> 2) & 0x3C));
    addr_ = (addr_ & 0x3FFF) | ((value & 7u) << 14);  // bit 2: A16, used in 128 KB mode
    // With DMA off CD5 does not latch, and a fill already armed stays armed; with it on, a
    // command without CD5 disarms it.
    if (!(regs_[1] & 0x10)) {
        code_ &= ~0x20;
        return 0;
    }
    if (!(code_ & 0x20)) {
        fillPending_ = false;
        return 0;
    }
    return startDma(clock);
}

uint32_t VdpMd::dmaLength() const {
    const uint32_t len = regs_[19] | (regs_[20] << 8);
    return len ? len : 0x10000;
}

int VdpMd::startDma(uint64_t clock) {
    const int type = regs_[23] >> 6;
    if (type == 2) {  // fill: starts with the next data port write
        fillPending_ = true;
        return 0;
    }
    fillPending_ = false;
    dmaLeft_ = dmaLength();
    regs_[19] = regs_[20] = 0;
    if (type == 3) {  // VRAM copy, byte by byte
        dmaMode_ = kDmaCopy;
        dmaSource_ = static_cast<uint32_t>(regs_[21] | (regs_[22] << 8));
        copyRead_ = false;
        const uint32_t end = (dmaSource_ + dmaLeft_) & 0xFFFF;
        regs_[21] = static_cast<uint8_t>(end);
        regs_[22] = static_cast<uint8_t>(end >> 8);
        if (clock == kUntimed) drain();
        return 0;
    }
    // 68000 to VRAM, CRAM or VSRAM, through the FIFO. The source wraps inside its 128 KB block.
    dmaMode_ = kDmaTransfer;
    dmaSource_ = (static_cast<uint32_t>(regs_[23] & 0x7F) << 17) | (regs_[22] << 9) | (regs_[21] << 1);
    const uint32_t end = (dmaSource_ & 0xFE0000) | ((dmaSource_ + dmaLeft_ * 2) & 0x1FFFF);
    regs_[21] = static_cast<uint8_t>(end >> 1);
    regs_[22] = static_cast<uint8_t>(end >> 9);
    const int perWord = (code_ & 0x0F) == 1 ? 2 : 1;
    const uint32_t queued = queueCount_;
    const bool instant = instantDma_ && dmaSource_ < 0x10000;
    refill();
    if (clock == kUntimed || instant) {
        drain();
        return 0;
    }
    // The 68000 stays off the bus until the last word is in the FIFO: one more goes in as
    // each write leaves it.
    uint64_t slots = 0;
    uint32_t leaving = dmaLeft_;
    for (uint32_t i = 0; i < queued && leaving > 0; ++i, --leaving) slots += static_cast<uint64_t>(entrySlots(queue_[i]));
    slots += static_cast<uint64_t>(leaving) * static_cast<uint64_t>(perWord);
    if (slots == 0) return 0;
    if (halfDone_) --slots;
    return static_cast<int>(slotAfter(clock, slots) - clock);
}

void VdpMd::refill() {
    while (dmaMode_ == kDmaTransfer && queueCount_ < 4) {
        const uint16_t word = dmaRead_ ? dmaRead_(dmaSource_) : 0;
        dmaSource_ = (dmaSource_ & 0xFE0000) | ((dmaSource_ + 2) & 0x1FFFF);
        push(word);
        if (--dmaLeft_ == 0) finishDma();
    }
}

void VdpMd::finishDma() {
    dmaMode_ = kDmaNone;
    dmaLeft_ = 0;
    code_ &= ~0x20;
}

void VdpMd::push(uint16_t value) {
    if (queueCount_ == queue_.size()) slot();  // never with the CPU held as it should be
    fifoData_[fifoNext_] = value;
    fifoNext_ = static_cast<uint8_t>((fifoNext_ + 1) & 3);
    queue_[queueCount_++] = FifoEntry{addr_, value, code_, false};
    addr_ = (addr_ + regs_[15]) & 0x1FFFF;
}

void VdpMd::storeWord(uint8_t code, uint32_t addr, uint16_t value) {
    switch (code & 0x0F) {
        case 1: {  // VRAM; an odd address stores the bytes swapped
            if (regs_[1] & 0x80) {
                // 128 KB mode, for a second bank of VRAM no console has: only the low byte
                // is stored, its address bits shuffled. What lands where comes from testpico.
                vram_[(addr & 0x7FFC) | ((addr >> 1) & 0x8000) | (((addr >> 1) & 1) ^ 1)] = static_cast<uint8_t>(value);
                break;
            }
            const uint32_t a = addr & 0xFFFF;
            if (a & 1) {
                vram_[a & ~1u] = static_cast<uint8_t>(value);
                vram_[a | 1] = static_cast<uint8_t>(value >> 8);
            } else {
                vram_[a] = static_cast<uint8_t>(value >> 8);
                vram_[a + 1] = static_cast<uint8_t>(value);
            }
            break;
        }
        case 3: {  // CRAM
            const int i = (addr >> 1) & 63;
            const uint32_t before = colors_[i];
            cram_[i] = value & 0x0EEE;
            const int r = (value >> 1) & 7, g = (value >> 5) & 7, b = (value >> 9) & 7;
            auto pack = [](int lr, int lg, int lb) {
                auto to8 = [](int l) { return static_cast<uint32_t>(l * 255 / 14); };
                return 0xFF000000u | (to8(lr) << 16) | (to8(lg) << 8) | to8(lb);
            };
            colors_[i] = pack(r * 2, g * 2, b * 2);
            colors_[64 + i] = pack(r, g, b);
            colors_[128 + i] = pack(7 + r, 7 + g, 7 + b);
            if (slotClock_ != kUntimed && cramWriteCount_ < static_cast<int>(cramWrites_.size()))
                cramWrites_[static_cast<size_t>(cramWriteCount_++)] = {slotClock_, static_cast<uint8_t>(i), before, colors_[i]};
            break;
        }
        case 5: {  // VSRAM
            const int i = (addr >> 1) & 63;
            if (i < 40) vsram_[i] = value & 0x07FF;
            break;
        }
        default: break;  // a read target, or none: the word is lost
    }
}

void VdpMd::slot() {
    if (queueCount_ > 0) {
        if (entrySlots(queue_[0]) == 2 && !halfDone_) {
            halfDone_ = true;
            return;
        }
        halfDone_ = false;
        const FifoEntry e = queue_[0];
        std::copy(queue_.begin() + 1, queue_.begin() + queueCount_, queue_.begin());
        --queueCount_;
        storeWord(e.code, e.addr, e.value);
        if (e.startsFill) {
            // The fill goes on from where the address is now. VRAM gets the written word's
            // high byte; CRAM and VSRAM get the FIFO's next entry, as the console does.
            dmaMode_ = kDmaFill;
            dmaLeft_ = dmaLength();
            fillValue_ = e.value;
            const uint32_t source = (regs_[21] | (regs_[22] << 8)) + dmaLeft_;
            regs_[21] = static_cast<uint8_t>(source);
            regs_[22] = static_cast<uint8_t>(source >> 8);
            regs_[19] = regs_[20] = 0;
        } else if (dmaMode_ == kDmaFill) {
            fillValue_ = e.value;  // a data port write during a fill changes what it fills with
        }
        refill();
        return;
    }
    if (dmaMode_ == kDmaFill) {
        const int target = code_ & 0x0F;
        // The high byte goes to the other byte of the word at the address: filling from
        // an odd address writes the high bytes of the words, from an even one the low.
        if (target == 1) vram_[(addr_ ^ 1) & 0xFFFF] = static_cast<uint8_t>(fillValue_ >> 8);
        else if (target == 3 || target == 5) storeWord(code_, addr_, fifoData_[fifoNext_]);
        addr_ = (addr_ + regs_[15]) & 0x1FFFF;
        if (--dmaLeft_ == 0) finishDma();
    } else if (dmaMode_ == kDmaCopy) {
        // A slot to read the byte and one to write it. Both addresses work on the bytes as
        // the VDP keeps them, the two of each word swapped.
        if (!copyRead_) {
            copyByte_ = vram_[(dmaSource_ ^ 1) & 0xFFFF];
            copyRead_ = true;
            return;
        }
        copyRead_ = false;
        vram_[(addr_ ^ 1) & 0xFFFF] = copyByte_;
        dmaSource_ = (dmaSource_ + 1) & 0xFFFF;
        addr_ = (addr_ + regs_[15]) & 0x1FFFF;
        if (--dmaLeft_ == 0) finishDma();
    }
}

void VdpMd::drain() {
    while (busy()) slot();
}

uint64_t VdpMd::slotsToDrain() const {
    uint64_t slots = 0;
    for (int i = 0; i < queueCount_; ++i) {
        slots += static_cast<uint64_t>(entrySlots(queue_[static_cast<size_t>(i)]));
        if (queue_[static_cast<size_t>(i)].startsFill) slots += dmaLength();
    }
    if (halfDone_) --slots;
    if (dmaMode_ == kDmaFill) slots += dmaLeft_;
    else if (dmaMode_ == kDmaCopy) slots += dmaLeft_ * 2 - (copyRead_ ? 1 : 0);
    else if (dmaMode_ == kDmaTransfer) slots += dmaLeft_ * static_cast<uint64_t>((code_ & 0x0F) == 1 ? 2 : 1);
    return slots;
}

namespace {

// The external access slots of a drawn line, in master clocks from its start (Kabuto's
// hardware notes: H40 `Hssss AsaaBsbb ((A~aaBSbb)*3 AraaBSbb)*5 ~~ s*23 ~ s*11`, an
// access every 16 clocks; H32 `... *4 ~~ s*13 ~ s*13 ~`, every 20). The picture part
// has three every 32 (H40) or 24 (H32) pixels; the blanking after it, those that fall
// while the next line's sprites are fetched.
constexpr uint16_t kH40Picture[] = {24,   152,  280,  536,  664,  792,  1048, 1176,
                                    1304, 1560, 1688, 1816, 2072, 2200, 2328};
constexpr uint16_t kH40Blank[] = {2568, 2584, 2968};
constexpr uint16_t kH32Picture[] = {30, 190, 350, 670, 830, 990, 1310, 1470, 1630, 1950, 2110, 2270};
constexpr uint16_t kH32Blank[] = {2570, 2590, 2870, 3150};

}  // namespace

VdpMd::SlotLayout VdpMd::slotLayout(int line) const {
    // The picture part of a line has the drawing pattern when the line is drawn; its
    // horizontal blanking, when the next one is (it fetches for that line). Otherwise
    // the slots are spread evenly. A transfer from the 68000 to CRAM or VSRAM, a word a
    // slot, loses 7 of them a line to the 68000's DRAM refresh.
    const bool display = displayEnabled();
    const int lines = linesPerFrame(), active = activeLines();
    const bool drawn = display && line < active;
    const bool nextDrawn = display && (line + 1 < active || line == lines - 1);
    const bool refreshGaps = dmaMode_ == kDmaTransfer && ((code_ & 0x0F) == 3 || (code_ & 0x0F) == 5);
    const int even = (h40() ? 205 : 167) - (refreshGaps ? 7 : 0);
    const int evenPicture = even * kActiveClocks / kMasterPerLine;
    SlotLayout l{evenPicture, even - evenPicture, nullptr, nullptr};
    if (drawn) {
        l.pictureAt = h40() ? kH40Picture : kH32Picture;
        l.picture = h40() ? 15 : 12;
    }
    if (nextDrawn) {
        l.blankAt = h40() ? kH40Blank : kH32Blank;
        l.blank = h40() ? 3 : 4;
    }
    return l;
}

namespace {

// Where slot i of a line falls, in master clocks from its start: at the drawing
// pattern's places, or spread evenly over its part.
uint64_t slotOffset(int i, VdpMd::SlotLayout l) {
    if (i < l.picture) {
        if (l.pictureAt) return l.pictureAt[i];
        return static_cast<uint64_t>(2 * i + 1) * VdpMd::kActiveClocks / static_cast<uint64_t>(2 * l.picture);
    }
    const int k = i - l.picture;
    if (l.blankAt) return l.blankAt[k];
    return VdpMd::kActiveClocks + static_cast<uint64_t>(2 * k + 1) * (VdpMd::kMasterPerLine - VdpMd::kActiveClocks) /
                                      static_cast<uint64_t>(2 * l.blank);
}

// The first slot of a line starting at `start` that falls after `clock` (all of them if
// none).
int firstSlotAfter(uint64_t start, VdpMd::SlotLayout l, uint64_t clock) {
    if (clock < start) return 0;
    const uint64_t rel = clock - start;
    int lo = 0, hi = l.picture + l.blank;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (slotOffset(mid, l) <= rel) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

}  // namespace

uint64_t VdpMd::slotAfter(uint64_t clock, uint64_t n) const {
    int line = slotLine_;
    uint64_t start = slotLineStart_;
    for (;;) {
        const SlotLayout layout = slotLayout(line);
        const int first = firstSlotAfter(start, layout, clock);
        const uint64_t here = static_cast<uint64_t>(layout.picture + layout.blank - first);
        if (n <= here) return start + slotOffset(first + static_cast<int>(n) - 1, layout);
        n -= here;
        start += kMasterPerLine;
        line = (line + 1) % linesPerFrame();
    }
}

void VdpMd::sync(uint64_t clock) {
    if (clock == kUntimed || clock <= syncedTo_) return;
    while (busy()) {
        const SlotLayout layout = slotLayout(slotLine_);
        const int per = layout.picture + layout.blank;
        const uint64_t lineEnd = slotLineStart_ + kMasterPerLine;
        const uint64_t limit = std::min(clock, lineEnd);
        for (int i = firstSlotAfter(slotLineStart_, layout, syncedTo_); i < per && busy(); ++i) {
            const uint64_t at = slotLineStart_ + slotOffset(i, layout);
            if (at > limit) break;
            slotClock_ = at;
            slot();
            slotClock_ = kUntimed;
            syncedTo_ = at;
        }
        if (clock < lineEnd) break;
        slotLineStart_ = lineEnd;
        slotLine_ = (slotLine_ + 1) % linesPerFrame();
        syncedTo_ = lineEnd;
    }
    // Nothing (more) to do: the slot clock just moves on.
    if (clock >= slotLineStart_ + kMasterPerLine) {
        const uint64_t lines = (clock - slotLineStart_) / kMasterPerLine;
        slotLineStart_ += lines * kMasterPerLine;
        slotLine_ = static_cast<int>((static_cast<uint64_t>(slotLine_) + lines) % static_cast<uint64_t>(linesPerFrame()));
    }
    syncedTo_ = clock;
}

int VdpMd::writeData(uint16_t value, uint64_t clock) {
    sync(clock);
    pendingControl_ = false;
    push(value);
    if (fillPending_) {
        fillPending_ = false;
        queue_[queueCount_ - 1u].startsFill = true;
    }
    if (clock == kUntimed) {
        drain();
        return 0;
    }
    if (queueCount_ <= 4) return 0;
    // The FIFO was full: the CPU waits until its oldest write is done.
    const uint64_t slots = static_cast<uint64_t>(entrySlots(queue_[0]) - (halfDone_ ? 1 : 0));
    return static_cast<int>(slotAfter(clock, slots) - clock);
}

uint16_t VdpMd::readData(uint64_t clock, int* wait) {
    sync(clock);
    pendingControl_ = false;
    // A read waits for the FIFO to empty (and a DMA to end), then takes a slot of its own.
    if (wait) *wait = clock == kUntimed ? 0 : static_cast<int>(slotAfter(clock, slotsToDrain() + 1) - clock);
    drain();
    // CRAM and VSRAM are narrower than the bus and the 8-bit VRAM read gives one byte: the
    // other bits come from the FIFO's next entry.
    const uint16_t stale = fifoData_[fifoNext_];
    uint16_t v = readBuffer_;
    switch (code_ & 0x0F) {
        case 0: {
            const uint32_t a = addr_ & 0xFFFE;
            v = static_cast<uint16_t>(vram_[a] << 8 | vram_[a + 1]);
            break;
        }
        case 4: {
            const int i = (addr_ >> 1) & 63;
            v = static_cast<uint16_t>((vsram_[i < 40 ? i : 0] & 0x07FF) | (stale & 0xF800));
            break;
        }
        case 8:
            v = static_cast<uint16_t>((cram_[(addr_ >> 1) & 63] & 0x0EEE) | (stale & 0xF111));
            break;
        case 12:  // undocumented 8-bit VRAM read
            v = static_cast<uint16_t>((stale & 0xFF00) | vram_[(addr_ ^ 1) & 0xFFFF]);
            break;
        default: break;
    }
    readBuffer_ = v;
    addr_ = (addr_ + regs_[15]) & 0x1FFFF;
    return v;
}

uint16_t VdpMd::readStatus(int lineClock, uint64_t clock) {
    sync(clock);
    uint16_t s = 0;
    if (queueCount_ == 0) s |= 0x200;
    if (queueCount_ >= 4) s |= 0x100;
    if (vintPending_) s |= 0x80;
    if (spriteOverflow_) s |= 0x40;
    if (spriteCollision_) s |= 0x20;
    if (oddFrame_ && (regs_[12] & 0x02)) s |= 0x10;
    // VBlank: from the V counter reaching the first line below the picture until it
    // reaches the frame's last line, which already fetches the sprites for line 0.
    const int v = counterLine(lineClock);
    if ((v >= activeLines() && v != linesPerFrame() - 1) || !displayEnabled()) s |= 0x08;
    // HBlank: set a little after the active part of the line ends and cleared just after
    // the next line starts (H40) or as it starts (H32). Games poll it to time writes.
    const int lc = lineClock % kMasterPerLine;
    const int start = kActiveClocks + (h40() ? 228 : 280);
    const int end = kActiveClocks + (h40() ? 872 : 860) - kMasterPerLine;
    if (lc >= start || lc < end) s |= 0x04;
    // DMA: a fill waiting for its data, or for that data to leave the FIFO, counts as
    // under way.
    bool dma = fillPending_ || dmaMode_ != kDmaNone;
    for (int i = 0; i < queueCount_; ++i) dma = dma || queue_[static_cast<size_t>(i)].startsFill;
    if (dma) s |= 0x02;
    if (pal_) s |= 0x01;
    pendingControl_ = false;
    spriteOverflow_ = spriteCollision_ = false;
    return s;
}

uint16_t VdpMd::readHV(int lineClock) const {
    if (regs_[0] & 0x02) return hvLatch_;
    int v = counterLine(lineClock);
    const bool v30 = activeLines() == 240;
    if (pal_) {
        const int jump = v30 ? 0x10A : 0x102;
        if (v > jump) v += (v30 ? 0x1D2 : 0x1CA) - (jump + 1);
    } else if (v > 0xEA) {
        v -= 6;
    }
    int h;
    if (h40()) {
        h = lineClock * 420 / kMasterPerLine / 2;
        if (h > 0xB6) h += 0xE4 - 0xB7;
    } else {
        h = lineClock * 342 / kMasterPerLine / 2;
        if (h > 0x93) h += 0xE9 - 0x94;
    }
    if ((regs_[12] & 0x06) == 0x06) v = (v << 1) | ((v >> 7) & 1);  // interlace mode 2
    // Bit 0 of the status shows hblank; the counters latch it implicitly here.
    return static_cast<uint16_t>(((v & 0xFF) << 8) | (h & 0xFF));
}

int VdpMd::counterLine(int lineClock) const {
    if (lineClock % kMasterPerLine >= kVcountClocks) return vcounter_;
    return (vcounter_ == 0 ? linesPerFrame() : vcounter_) - 1;
}

void VdpMd::startLine(int line, uint64_t clock) {
    if (clock != kUntimed) {
        if (clock >= syncedTo_ || clock + kMasterPerLine <= syncedTo_) {
            // The clock going back is a reset or a state loaded: what was waiting is done now.
            if (clock >= syncedTo_) sync(clock);
            else drain();
            slotLineStart_ = syncedTo_ = clock;
        }
        // Otherwise a write that waited past the end of the last line has already taken
        // this line's first slots, and they stay taken.
        slotLine_ = line;
    }
    vcounter_ = line;
    cramWriteCount_ = 0;
    if (line == activeLines() + 1) z80Irq_ = false;  // the Z80's interrupt line is held for about one line
}

void VdpMd::raiseVint() {
    vintPending_ = true;
    z80Irq_ = true;
}

void VdpMd::endLine(int line) {
    if (line <= activeLines()) {
        if (--hintCounter_ < 0) {
            hintCounter_ = regs_[10];
            hintPending_ = true;
        }
    } else {
        hintCounter_ = regs_[10];
    }
}

void VdpMd::endFrame() {
    ++frameNumber_;
    frame_->frameNumber = frameNumber_;
    oddFrame_ = (regs_[12] & 0x02) ? !oddFrame_ : false;
}

int VdpMd::irqLevel() const {
    if (vintPending_ && (regs_[1] & 0x20)) return 6;
    if (hintPending_ && (regs_[0] & 0x10)) return 4;
    return 0;
}

void VdpMd::acknowledge(int level) {
    if (level != 4 && level != 6) return;
    // The acknowledge clears whichever interrupt is pending and enabled now, VINT first,
    // whichever of the two the 68000 took: one disabled since it was raised stays pending.
    if (vintPending_ && (regs_[1] & 0x20)) vintPending_ = false;
    else if (hintPending_ && (regs_[0] & 0x10)) hintPending_ = false;
}

int VdpMd::wideColumnsFor(int aspectW, int aspectH) {
    if (aspectW <= 0 || aspectH <= 0) return 0;
    // The console's picture is 4:3; each side takes half of what is missing:
    // 320 * (W:H / 4:3 - 1) / 2, rounded.
    const int extra = (320 * (3 * aspectW - 4 * aspectH) + 4 * aspectH) / (8 * aspectH);
    return std::clamp(extra, 0, VideoFrameMd::kMaxWideSide);
}

void VdpMd::drawPlane(int line, bool planeA, int left, int right, LineCells& out,
                      std::array<bool, VideoFrameMd::kMaxWidth>& isWindow) {
    const int width = h40() ? 320 : 256;
    const bool im2 = (regs_[12] & 0x06) == 0x06;
    const int tileRows = im2 ? 16 : 8;
    const int y = im2 ? line * 2 + (oddFrame_ ? 1 : 0) : line;
    const int cellsW = kPlaneCells[regs_[16] & 3];
    const int cellsH = kPlaneCells[(regs_[16] >> 4) & 3];
    const int planeW = cellsW * 8, planeH = cellsH * tileRows;
    const uint32_t base = planeA ? (regs_[2] & 0x38u) << 10 : (regs_[4] & 0x07u) << 13;

    const uint32_t hsBase = (regs_[13] & 0x3Fu) << 10;
    const int hsMode = regs_[11] & 3;
    const int hsOffset = hsMode == 0 ? 0 : hsMode == 1 ? (line & 7) * 4 : hsMode == 2 ? (line & ~7) * 4 : line * 4;
    const uint32_t hsAddr = (hsBase + static_cast<uint32_t>(hsOffset) + (planeA ? 0 : 2)) & 0xFFFE;
    const int hscroll = (vram_[hsAddr] << 8 | vram_[hsAddr + 1]) & 0x3FF;
    if (planeA) frame_->lines[std::min(y, VideoFrameMd::kMaxHeight - 1)].hscrollA = static_cast<int16_t>(hscroll);
    else frame_->lines[std::min(y, VideoFrameMd::kMaxHeight - 1)].hscrollB = static_cast<int16_t>(hscroll);
    const bool columnScroll = (regs_[11] & 0x04) != 0;

    // The window replaces plane A on the lines above/below regs 18 and the columns
    // left/right of reg 17.
    bool lineInWindow = false;
    int winLeft = 0, winRight = 0;
    uint32_t winBase = 0;
    int winCells = 0;
    if (planeA) {
        const int vpos = (regs_[18] & 0x1F) * 8;
        lineInWindow = (regs_[18] & 0x80) ? line >= vpos : line < vpos;
        const int hpos = (regs_[17] & 0x1F) * 16;
        if (regs_[17] & 0x80) {
            winLeft = hpos;
            winRight = width;
        } else {
            winLeft = 0;
            winRight = hpos;
        }
        winBase = (regs_[3] & (h40() ? 0x3Cu : 0x3Eu)) << 10;
        winCells = h40() ? 64 : 32;
    }

    auto fetch = [&](uint32_t nameAddr, int px, int py, Cell& cell) {
        nameAddr &= 0xFFFE;
        const uint16_t entry = static_cast<uint16_t>(vram_[nameAddr] << 8 | vram_[nameAddr + 1]);
        const uint16_t tile = entry & 0x7FF;
        int tx = px & 7, ty = py % tileRows;
        if (entry & 0x0800) tx = 7 - tx;
        if (entry & 0x1000) ty = tileRows - 1 - ty;
        const uint32_t pattern = im2 ? (static_cast<uint32_t>(tile & 0x3FF) << 6) : (static_cast<uint32_t>(tile) << 5);
        const uint8_t byte = vram_[(pattern + static_cast<uint32_t>(ty * 4 + tx / 2)) & 0xFFFF];
        const int index = (tx & 1) ? (byte & 15) : (byte >> 4);
        cell.color = static_cast<uint8_t>(((entry >> 13) & 3) * 16 + index);
        cell.priority = (entry & 0x8000) != 0;
        cell.tile = tile;
    };

    for (int x = -left; x < width + right; ++x) {
        const int i = x + left;
        // Past the edges (widescreen) the window goes on as at the nearest edge.
        const int wx = std::clamp(x, 0, width - 1);
        if (planeA && (lineInWindow || (wx >= winLeft && wx < winRight))) {
            const int cell = (x >> 3) & (winCells - 1);
            const uint32_t addr = winBase + static_cast<uint32_t>(((y / tileRows) * winCells + cell) * 2);
            fetch(addr, x, y, out[i]);
            isWindow[i] = true;
            continue;
        }
        isWindow[i] = false;
        const int column = std::clamp(x >> 4, 0, 19);
        const int vs = (columnScroll ? vsram_[column * 2 + (planeA ? 0 : 1)] : vsram_[planeA ? 0 : 1]) & (im2 ? 0x7FF : 0x3FF);
        const int py = (y + vs) & (planeH - 1);
        const int px = (x - hscroll) & (planeW - 1);
        const uint32_t addr = base + static_cast<uint32_t>(((py / tileRows) * cellsW + px / 8) * 2);
        fetch(addr, px, py, out[i]);
    }
}

void VdpMd::drawSprites(int line, int left, int right, LineCells& out, std::array<uint8_t, VideoFrameMd::kMaxWidth>& number) {
    const bool wide = h40();
    const int width = wide ? 320 : 256;
    const int maxSprites = wide ? 80 : 64;
    const int maxPerLine = wide ? 20 : 16;
    const int maxPixels = wide ? 320 : 256;
    const bool im2 = (regs_[12] & 0x06) == 0x06;
    const int tileRows = im2 ? 16 : 8;
    const uint32_t satb = (regs_[5] & (wide ? 0x7Eu : 0x7Fu)) << 9;
    const int yLine = im2 ? line * 2 + (oddFrame_ ? 1 : 0) + 256 : line + 128;

    for (auto& c : out) c = {0, false, 0};

    struct Found {
        int index, x, y, hcells, vcells;
        uint16_t attr;
    };
    // Without the limit every sprite on the line is drawn, but the flags stay as the real
    // VDP leaves them: `hwCount` sprites, and the cells up to its pixel budget, are what the
    // console would draw, and only they set the overflow and collision bits. The game sees
    // the same console either way; only the picture changes.
    std::array<Found, 80> found{};
    int count = 0, hwCount = -1;
    int link = 0;
    for (int n = 0; n < maxSprites; ++n) {
        const uint32_t a = (satb + static_cast<uint32_t>(link) * 8) & 0xFFFF;
        const int sy = (vram_[a] << 8 | vram_[(a + 1) & 0xFFFF]) & (im2 ? 0x3FF : 0x1FF);
        const uint8_t size = vram_[(a + 2) & 0xFFFF];
        const int next = vram_[(a + 3) & 0xFFFF] & 0x7F;
        const int hcells = ((size >> 2) & 3) + 1, vcells = (size & 3) + 1;
        if (yLine >= sy && yLine < sy + vcells * tileRows) {
            if (count == maxPerLine && hwCount < 0) {
                spriteOverflow_ = true;
                hwCount = count;
                if (!unlimitedSprites_) break;
            }
            const uint16_t attr = static_cast<uint16_t>(vram_[(a + 4) & 0xFFFF] << 8 | vram_[(a + 5) & 0xFFFF]);
            const int sx = (vram_[(a + 6) & 0xFFFF] << 8 | vram_[(a + 7) & 0xFFFF]) & 0x1FF;
            found[count++] = {link, sx, sy, hcells, vcells, attr};
        }
        if (next == 0 || next >= maxSprites) break;
        link = next;
    }
    if (hwCount < 0) hwCount = count;

    int pixels = 0;
    bool beyond = false;  // past what the real VDP would draw on this line
    bool masked = false, sawNonZeroX = false;
    // A sprite at x = 0 hides the lower-priority sprites after it on this line, once a
    // sprite at another x came before it, or from the first one if the line before ran
    // out of sprite pixels.
    const bool maskFromStart = dotOverflow_;
    dotOverflow_ = false;
    for (int i = 0; i < count; ++i) {
        const Found& s = found[i];
        if (s.x == 0 && (sawNonZeroX || maskFromStart)) masked = true;
        if (s.x != 0) sawNonZeroX = true;
        const bool hflip = (s.attr & 0x0800) != 0, vflip = (s.attr & 0x1000) != 0;
        const int height = s.vcells * tileRows;
        int row = yLine - s.y;
        if (vflip) row = height - 1 - row;
        if (i == hwCount) beyond = true;
        for (int cx = 0; cx < s.hcells; ++cx) {
            if (pixels >= maxPixels && !beyond) {
                spriteOverflow_ = true;
                dotOverflow_ = true;
                if (!unlimitedSprites_) return;
                beyond = true;
            }
            pixels += 8;
            if (masked) continue;
            const int col = hflip ? s.hcells - 1 - cx : cx;
            const uint32_t tile = static_cast<uint32_t>((s.attr & 0x7FF) + col * s.vcells + row / tileRows) & 0x7FF;
            const uint32_t pattern = im2 ? ((tile & 0x3FF) << 6) : (tile << 5);
            const int ty = row % tileRows;
            for (int px = 0; px < 8; ++px) {
                const int x = s.x - 128 + cx * 8 + px;
                if (x < -left || x >= width + right) continue;
                const int at = x + left;
                const int tx = hflip ? 7 - px : px;
                const uint8_t byte = vram_[(pattern + static_cast<uint32_t>(ty * 4 + tx / 2)) & 0xFFFF];
                const int index = (tx & 1) ? (byte & 15) : (byte >> 4);
                if (!index) continue;
                if (out[at].color & 15) {
                    // The console only sees collisions in its own picture.
                    if (!beyond && x >= 0 && x < width) spriteCollision_ = true;
                    continue;
                }
                out[at].color = static_cast<uint8_t>(((s.attr >> 13) & 3) * 16 + index);
                out[at].priority = (s.attr & 0x8000) != 0;
                out[at].tile = static_cast<uint16_t>(tile);
                number[at] = static_cast<uint8_t>(s.index);
            }
        }
    }
}

void VdpMd::renderLine(int line) {
    VideoFrameMd& f = *frame_;
    const int width = h40() ? 320 : 256;
    // The widescreen columns cover the same part of the screen in H32 as in H40.
    const int left = (wideLeft_ * width + 160) / 320, right = (wideRight_ * width + 160) / 320;
    const int total = width + left + right;
    const bool im2 = (regs_[12] & 0x06) == 0x06;
    if (line == 0) {
        f.width = total;
        f.wideLeft = left;
        f.wideRight = right;
        f.height = activeLines() * (im2 ? 2 : 1);
        f.interlaced = im2;
    }
    const int row = im2 ? line * 2 + (oddFrame_ ? 1 : 0) : line;
    if (row >= VideoFrameMd::kMaxHeight) return;
    const int o = row * VideoFrameMd::kMaxWidth;
    const int backdropIndex = regs_[7] & 0x3F;
    const bool sh = (regs_[12] & 0x08) != 0;
    f.lines[row].displayEnabled = displayEnabled();

    if (!displayEnabled()) {
        uint32_t c = color(backdropIndex, MdShade::Normal);
        // The backdrop's writes during the picture: the line starts with the colour from
        // before the first, and each one shows from its pixel on.
        const uint64_t start = slotLineStart_;
        int first = 0;
        while (first < cramWriteCount_ &&
               (cramWrites_[static_cast<size_t>(first)].clock < start || cramWrites_[static_cast<size_t>(first)].index != backdropIndex))
            ++first;
        if (first < cramWriteCount_) c = cramWrites_[static_cast<size_t>(first)].before;
        f.backdrop[row] = c;
        int next = first;
        for (int x = 0; x < VideoFrameMd::kMaxWidth; ++x) {
            for (; next < cramWriteCount_; ++next) {
                const CramWrite& w = cramWrites_[static_cast<size_t>(next)];
                if (w.clock < start || w.index != backdropIndex) continue;
                const int at = left + static_cast<int>((w.clock - start) * static_cast<uint64_t>(width) / kActiveClocks);
                if (at > x) break;
                c = w.after;
            }
            f.rgba[o + x] = c;
            f.info[o + x] = MdPixel{static_cast<uint8_t>(backdropIndex), MdLayer::Backdrop, false, MdShade::Normal, 0};
            f.planeA[o + x] = f.planeB[o + x] = f.sprites[o + x] = 0;
            f.planeAInfo[o + x] = f.planeBInfo[o + x] = f.spriteInfo[o + x] = MdPixel{};
        }
        return;
    }

    LineCells b, a, s;
    std::array<bool, VideoFrameMd::kMaxWidth> isWindow{}, unusedWindow{};
    std::array<uint8_t, VideoFrameMd::kMaxWidth> spriteNumber{};
    drawPlane(line, false, left, right, b, unusedWindow);
    drawPlane(line, true, left, right, a, isWindow);
    drawSprites(line, left, right, s, spriteNumber);

    f.backdrop[row] = color(backdropIndex, sh ? MdShade::Shadow : MdShade::Normal);
    for (int x = 0; x < total; ++x) {
        const Cell& B = b[x];
        const Cell& A = a[x];
        const Cell& S = s[x];
        const bool bOpaque = (B.color & 15) != 0, aOpaque = (A.color & 15) != 0, sOpaque = (S.color & 15) != 0;
        const MdLayer aLayer = isWindow[x] ? MdLayer::Window : MdLayer::PlaneA;
        const bool shadowed = sh && !B.priority && !A.priority;
        const bool sOperator = sh && sOpaque && (S.color == 0x3E || S.color == 0x3F);

        // Hardware priority, front to back: S high, A high, B high, S low, A low, B low.
        enum Pick { PickS, PickA, PickB, PickBackdrop };
        auto pickPlanes = [&](bool highOnly) {
            if (aOpaque && A.priority) return PickA;
            if (bOpaque && B.priority) return PickB;
            if (highOnly) return PickBackdrop;
            if (aOpaque) return PickA;
            if (bOpaque) return PickB;
            return PickBackdrop;
        };
        Pick pick;
        if (sOpaque && S.priority) pick = PickS;
        else if (aOpaque && A.priority) pick = PickA;
        else if (bOpaque && B.priority) pick = PickB;
        else if (sOpaque) pick = PickS;
        else pick = pickPlanes(false);

        MdShade shade = shadowed ? MdShade::Shadow : MdShade::Normal;
        if (pick == PickS) {
            if (sOperator) {
                // Palette 3 colours 14/15 do not draw: they highlight or shadow what lies below.
                pick = pickPlanes(false);
                if (S.color == 0x3F) shade = MdShade::Shadow;
                else shade = shadowed ? MdShade::Normal : MdShade::Highlight;
            } else if (S.priority) {
                shade = MdShade::Normal;
            }
        }

        MdPixel px;
        uint32_t rgb;
        switch (pick) {
            case PickS:
                px = {S.color, MdLayer::Sprite, S.priority, shade, spriteNumber[x]};
                rgb = color(S.color, shade);
                break;
            case PickA:
                px = {A.color, aLayer, A.priority, shade, A.tile};
                rgb = color(A.color, shade);
                break;
            case PickB:
                px = {B.color, MdLayer::PlaneB, B.priority, shade, B.tile};
                rgb = color(B.color, shade);
                break;
            default:
                px = {static_cast<uint8_t>(backdropIndex), MdLayer::Backdrop, false, shade, 0};
                rgb = color(backdropIndex, shade);
                break;
        }
        f.rgba[o + x] = rgb;
        f.info[o + x] = px;

        // The layers on their own, with the shading each would show where it is visible.
        const MdShade planeShade = shadowed ? MdShade::Shadow : MdShade::Normal;
        const MdShade bShade = pick == PickB ? shade : planeShade;
        const MdShade aShade = pick == PickA ? shade : planeShade;
        f.planeB[o + x] = bOpaque ? color(B.color, bShade) : 0;
        f.planeBInfo[o + x] = {B.color, MdLayer::PlaneB, B.priority, bShade, B.tile};
        f.planeA[o + x] = aOpaque ? color(A.color, aShade) : 0;
        f.planeAInfo[o + x] = {A.color, aLayer, A.priority, aShade, A.tile};
        const MdShade sShade = (S.priority || !shadowed) ? MdShade::Normal : MdShade::Shadow;
        f.sprites[o + x] = (sOpaque && !sOperator) ? color(S.color, sShade) : 0;
        f.spriteInfo[o + x] = {S.color, MdLayer::Sprite, S.priority, sShade, spriteNumber[x]};
    }
    for (int x = total; x < VideoFrameMd::kMaxWidth; ++x) {
        f.rgba[o + x] = f.planeA[o + x] = f.planeB[o + x] = f.sprites[o + x] = 0;
        f.info[o + x] = f.planeAInfo[o + x] = f.planeBInfo[o + x] = f.spriteInfo[o + x] = MdPixel{};
    }
}

}  // namespace plumsg
