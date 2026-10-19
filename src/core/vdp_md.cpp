#include "vdp_md.h"

#include <algorithm>

namespace plumsg {

namespace {

constexpr int kPlaneCells[4] = {32, 64, 32, 128};  // size code 2 is invalid; acts as 32

// 68000 DMA bandwidth in bytes per line: {active display, blanking}, for H32 and H40.
constexpr int kDmaBytesPerLine[2][2] = {{16, 167}, {18, 205}};

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
    vcounter_ = 0;
    hintCounter_ = 0;
    vintPending_ = hintPending_ = z80Irq_ = false;
    spriteOverflow_ = spriteCollision_ = false;
    oddFrame_ = false;
    inVblank_ = false;
    dmaBusyLines_ = 0;
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
    if (reg >= 24) return;
    regs_[reg] = value;
}

int VdpMd::writeControl(uint16_t value) {
    if (!pendingControl_) {
        if ((value & 0xC000) == 0x8000) {
            writeRegister((value >> 8) & 0x1F, static_cast<uint8_t>(value));
            return 0;
        }
        code_ = static_cast<uint8_t>((code_ & 0x3C) | (value >> 14));
        addr_ = (addr_ & 0xC000) | (value & 0x3FFF);
        pendingControl_ = true;
        return 0;
    }
    pendingControl_ = false;
    code_ = static_cast<uint8_t>((code_ & 0x03) | ((value >> 2) & 0x3C));
    addr_ = (addr_ & 0x3FFF) | ((value & 3u) << 14);
    if ((code_ & 0x20) && (regs_[1] & 0x10)) return runDma();
    return 0;
}

uint32_t VdpMd::dmaLength() const {
    const uint32_t len = regs_[19] | (regs_[20] << 8);
    return len ? len : 0x10000;
}

int VdpMd::runDma() {
    const int type = regs_[23] >> 6;
    if (type == 2) {  // fill: starts with the next data port write
        fillPending_ = true;
        return 0;
    }
    const uint32_t length = dmaLength();
    const bool blank = inVblank_ || !displayEnabled();
    const int rate = kDmaBytesPerLine[h40() ? 1 : 0][blank ? 1 : 0];
    if (type == 3) {  // VRAM copy, byte by byte
        uint16_t src = static_cast<uint16_t>(regs_[21] | (regs_[22] << 8));
        for (uint32_t i = 0; i < length; ++i) {
            vram_[addr_ & 0xFFFF] = vram_[src];
            ++src;
            addr_ = (addr_ + regs_[15]) & 0xFFFF;
        }
        regs_[21] = static_cast<uint8_t>(src);
        regs_[22] = static_cast<uint8_t>(src >> 8);
        regs_[19] = regs_[20] = 0;
        dmaBusyLines_ = static_cast<int>(length * 2 / static_cast<uint32_t>(rate)) + 1;
        code_ &= ~0x20;
        return 0;
    }
    // 68000 to VRAM, CRAM or VSRAM. The source wraps inside its 128 KB block.
    uint32_t src = (static_cast<uint32_t>(regs_[23] & 0x7F) << 17) | (regs_[22] << 9) | (regs_[21] << 1);
    for (uint32_t i = 0; i < length; ++i) {
        const uint16_t word = dmaRead_ ? dmaRead_(src) : 0;
        writeWord(word);
        src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF);
    }
    regs_[21] = static_cast<uint8_t>(src >> 1);
    regs_[22] = static_cast<uint8_t>(src >> 9);
    regs_[19] = regs_[20] = 0;
    code_ &= ~0x20;
    // Each VRAM word takes two byte slots; CRAM and VSRAM take one.
    const uint32_t units = (code_ & 0x0F) == 1 ? length * 2 : length;
    return static_cast<int>(units * kMasterPerLine / static_cast<uint32_t>(rate));
}

void VdpMd::dmaFill(uint16_t value) {
    const uint32_t length = dmaLength();
    const int target = code_ & 0x0F;
    for (uint32_t i = 0; i < length; ++i) {
        // The high byte goes to the other byte of the word at the address: filling from
        // an odd address writes the high bytes of the words, from an even one the low.
        if (target == 1) vram_[(addr_ ^ 1) & 0xFFFF] = static_cast<uint8_t>(value >> 8);
        else if (target == 3) cram_[(addr_ >> 1) & 63] = value & 0x0EEE;
        else if (target == 5 && ((addr_ >> 1) & 63) < 40) vsram_[(addr_ >> 1) & 63] = value & 0x07FF;
        addr_ = (addr_ + regs_[15]) & 0xFFFF;
    }
    if (target == 3) cacheColors();
    regs_[19] = regs_[20] = 0;
    const int rate = kDmaBytesPerLine[h40() ? 1 : 0][(inVblank_ || !displayEnabled()) ? 1 : 0];
    dmaBusyLines_ = static_cast<int>(length / static_cast<uint32_t>(rate)) + 1;
    code_ &= ~0x20;
}

void VdpMd::writeWord(uint16_t value) {
    switch (code_ & 0x0F) {
        case 1: {  // VRAM; an odd address stores the bytes swapped
            const uint32_t a = addr_ & 0xFFFF;
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
            const int i = (addr_ >> 1) & 63;
            cram_[i] = value & 0x0EEE;
            const int r = (value >> 1) & 7, g = (value >> 5) & 7, b = (value >> 9) & 7;
            auto pack = [](int lr, int lg, int lb) {
                auto to8 = [](int l) { return static_cast<uint32_t>(l * 255 / 14); };
                return 0xFF000000u | (to8(lr) << 16) | (to8(lg) << 8) | to8(lb);
            };
            colors_[i] = pack(r * 2, g * 2, b * 2);
            colors_[64 + i] = pack(r, g, b);
            colors_[128 + i] = pack(7 + r, 7 + g, 7 + b);
            break;
        }
        case 5: {  // VSRAM
            const int i = (addr_ >> 1) & 63;
            if (i < 40) vsram_[i] = value & 0x07FF;
            break;
        }
        default: break;  // the port is set up for reading
    }
    addr_ = (addr_ + regs_[15]) & 0xFFFF;
}

int VdpMd::writeData(uint16_t value) {
    pendingControl_ = false;
    writeWord(value);
    if (fillPending_) {
        fillPending_ = false;
        dmaFill(value);
    }
    return 0;
}

uint16_t VdpMd::readData() {
    pendingControl_ = false;
    uint16_t v = readBuffer_;
    switch (code_ & 0x0F) {
        case 0: {
            const uint32_t a = addr_ & 0xFFFE;
            v = static_cast<uint16_t>(vram_[a] << 8 | vram_[a + 1]);
            break;
        }
        case 4: {
            const int i = (addr_ >> 1) & 63;
            v = static_cast<uint16_t>((vsram_[i < 40 ? i : 0] & 0x07FF) | (readBuffer_ & 0xF800));
            break;
        }
        case 8:
            v = static_cast<uint16_t>((cram_[(addr_ >> 1) & 63] & 0x0EEE) | (readBuffer_ & 0xF111));
            break;
        case 12:  // undocumented 8-bit VRAM read
            v = static_cast<uint16_t>((readBuffer_ & 0xFF00) | vram_[(addr_ ^ 1) & 0xFFFF]);
            break;
        default: break;
    }
    readBuffer_ = v;
    addr_ = (addr_ + regs_[15]) & 0xFFFF;
    return v;
}

uint16_t VdpMd::readStatus(int lineClock) {
    uint16_t s = 0x3600;  // FIFO empty; bits 10-15 read as the open bus usually holds them
    if (vintPending_) s |= 0x80;
    if (spriteOverflow_) s |= 0x40;
    if (spriteCollision_) s |= 0x20;
    if (oddFrame_ && (regs_[12] & 0x02)) s |= 0x10;
    if (inVblank_ || !displayEnabled()) s |= 0x08;
    // HBlank: set a little after the active part of the line ends and cleared just after
    // the next line starts (H40) or as it starts (H32). Games poll it to time writes.
    const int clock = lineClock % kMasterPerLine;
    const int start = kActiveClocks + (h40() ? 228 : 280);
    const int end = kActiveClocks + (h40() ? 872 : 860) - kMasterPerLine;
    if (clock >= start || clock < end) s |= 0x04;
    if (dmaBusyLines_ > 0) s |= 0x02;
    if (pal_) s |= 0x01;
    pendingControl_ = false;
    spriteOverflow_ = spriteCollision_ = false;
    return s;
}

uint16_t VdpMd::readHV(int lineClock) const {
    int v = vcounter_;
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

void VdpMd::startLine(int line) {
    vcounter_ = line;
    const int active = activeLines();
    if (line == 0) inVblank_ = false;
    if (line == active) {
        inVblank_ = true;
    } else if (line == active + 1) {
        z80Irq_ = false;  // the Z80's interrupt line is held for about one line
    }
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
    if (dmaBusyLines_ > 0) --dmaBusyLines_;
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
    if (level == 6) vintPending_ = false;
    else if (level == 4) hintPending_ = false;
}

void VdpMd::drawPlane(int line, bool planeA, std::array<Cell, 320>& out, std::array<bool, 320>& isWindow) {
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

    for (int x = 0; x < width; ++x) {
        if (planeA && (lineInWindow || (x >= winLeft && x < winRight))) {
            const uint32_t addr = winBase + static_cast<uint32_t>(((y / tileRows) * winCells + x / 8) * 2);
            fetch(addr, x, y, out[x]);
            isWindow[x] = true;
            continue;
        }
        isWindow[x] = false;
        const int column = std::min(x / 16, 19);
        const int vs = (columnScroll ? vsram_[column * 2 + (planeA ? 0 : 1)] : vsram_[planeA ? 0 : 1]) & (im2 ? 0x7FF : 0x3FF);
        const int py = (y + vs) & (planeH - 1);
        const int px = (x - hscroll) & (planeW - 1);
        const uint32_t addr = base + static_cast<uint32_t>(((py / tileRows) * cellsW + px / 8) * 2);
        fetch(addr, px, py, out[x]);
    }
}

void VdpMd::drawSprites(int line, std::array<Cell, 320>& out, std::array<uint8_t, 320>& number) {
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
    for (int i = 0; i < count; ++i) {
        const Found& s = found[i];
        // A sprite at x = 0 hides the lower-priority sprites after it on this line.
        if (s.x == 0 && sawNonZeroX) masked = true;
        if (s.x != 0) sawNonZeroX = true;
        const bool hflip = (s.attr & 0x0800) != 0, vflip = (s.attr & 0x1000) != 0;
        const int height = s.vcells * tileRows;
        int row = yLine - s.y;
        if (vflip) row = height - 1 - row;
        if (i == hwCount) beyond = true;
        for (int cx = 0; cx < s.hcells; ++cx) {
            if (pixels >= maxPixels && !beyond) {
                spriteOverflow_ = true;
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
                if (x < 0 || x >= width) continue;
                const int tx = hflip ? 7 - px : px;
                const uint8_t byte = vram_[(pattern + static_cast<uint32_t>(ty * 4 + tx / 2)) & 0xFFFF];
                const int index = (tx & 1) ? (byte & 15) : (byte >> 4);
                if (!index) continue;
                if (out[x].color & 15) {
                    if (!beyond) spriteCollision_ = true;
                    continue;
                }
                out[x].color = static_cast<uint8_t>(((s.attr >> 13) & 3) * 16 + index);
                out[x].priority = (s.attr & 0x8000) != 0;
                out[x].tile = static_cast<uint16_t>(tile);
                number[x] = static_cast<uint8_t>(s.index);
            }
        }
    }
}

void VdpMd::renderLine(int line) {
    VideoFrameMd& f = *frame_;
    const int width = h40() ? 320 : 256;
    const bool im2 = (regs_[12] & 0x06) == 0x06;
    if (line == 0) {
        f.width = width;
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
        const uint32_t c = color(backdropIndex, MdShade::Normal);
        f.backdrop[row] = c;
        for (int x = 0; x < VideoFrameMd::kMaxWidth; ++x) {
            f.rgba[o + x] = c;
            f.info[o + x] = MdPixel{static_cast<uint8_t>(backdropIndex), MdLayer::Backdrop, false, MdShade::Normal, 0};
            f.planeA[o + x] = f.planeB[o + x] = f.sprites[o + x] = 0;
            f.planeAInfo[o + x] = f.planeBInfo[o + x] = f.spriteInfo[o + x] = MdPixel{};
        }
        return;
    }

    std::array<Cell, 320> b, a, s;
    std::array<bool, 320> isWindow{}, unusedWindow{};
    std::array<uint8_t, 320> spriteNumber{};
    drawPlane(line, false, b, unusedWindow);
    drawPlane(line, true, a, isWindow);
    drawSprites(line, s, spriteNumber);

    f.backdrop[row] = color(backdropIndex, sh ? MdShade::Shadow : MdShade::Normal);
    for (int x = 0; x < width; ++x) {
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
    for (int x = width; x < VideoFrameMd::kMaxWidth; ++x) {
        f.rgba[o + x] = f.planeA[o + x] = f.planeB[o + x] = f.sprites[o + x] = 0;
        f.info[o + x] = f.planeAInfo[o + x] = f.planeBInfo[o + x] = f.spriteInfo[o + x] = MdPixel{};
    }
}

}  // namespace plumsg
