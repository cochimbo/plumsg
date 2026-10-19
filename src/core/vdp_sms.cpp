#include "vdp_sms.h"

namespace plumsg {

void VdpSms::reset() {
    vram_.fill(0);
    cram_.fill(0);
    // What the BIOS leaves in the registers before it starts the cartridge: some games
    // count on it, since they do not set every register.
    static constexpr uint8_t kBiosRegs[16] = {0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00,
                                              0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (size_t i = 0; i < 16; ++i) regs_[i] = kBiosRegs[i];
    pendingControl_ = false;
    code_ = 0;
    addr_ = 0;
    latch_ = 0;
    readBuffer_ = 0;
    line_ = 0;
    lineCounter_ = 0xFF;
    frameIrq_ = lineIrq_ = false;
    spriteOverflow_ = spriteCollision_ = false;
    vscrollLatch_ = 0;
    cacheColors();
}

void VdpSms::cacheColors() {
    for (size_t i = 0; i < 32; ++i) {
        const uint8_t c = cram_[i];
        const uint32_t r = (c & 3) * 85u, g = ((c >> 2) & 3) * 85u, b = ((c >> 4) & 3) * 85u;
        colors_[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
}

uint8_t VdpSms::readData() {
    pendingControl_ = false;
    const uint8_t value = readBuffer_;
    readBuffer_ = vram_[addr_];
    addr_ = (addr_ + 1) & 0x3FFF;
    return value;
}

void VdpSms::writeData(uint8_t value) {
    pendingControl_ = false;
    if (code_ == 3) {
        cram_[addr_ & 0x1F] = value & 0x3F;
        cacheColors();
    } else {
        vram_[addr_] = value;
    }
    readBuffer_ = value;
    addr_ = (addr_ + 1) & 0x3FFF;
}

uint8_t VdpSms::readStatus() {
    const uint8_t value = static_cast<uint8_t>((frameIrq_ ? 0x80 : 0) | (spriteOverflow_ ? 0x40 : 0) |
                                               (spriteCollision_ ? 0x20 : 0) | 0x1F);
    frameIrq_ = lineIrq_ = false;
    spriteOverflow_ = spriteCollision_ = false;
    pendingControl_ = false;
    return value;
}

void VdpSms::writeControl(uint8_t value) {
    if (!pendingControl_) {
        latch_ = value;
        addr_ = static_cast<uint16_t>((addr_ & 0x3F00) | value);
        pendingControl_ = true;
        return;
    }
    pendingControl_ = false;
    code_ = value >> 6;
    addr_ = static_cast<uint16_t>((value & 0x3F) << 8 | latch_);
    if (code_ == 0) {
        // A read command fetches the first byte at once.
        readBuffer_ = vram_[addr_];
        addr_ = (addr_ + 1) & 0x3FFF;
    } else if (code_ == 2) {
        regs_[value & 0x0F] = latch_;
    }
}

int VdpSms::activeLines() const {
    // Mode 4 with M2 set picks the taller pictures by M1 (224) or M3 (240).
    if ((regs_[0] & 0x06) == 0x06) {
        if (regs_[1] & 0x10) return 224;
        if (regs_[1] & 0x08) return 240;
    }
    return 192;
}

uint8_t VdpSms::vcounter() const {
    // The counter jumps back once a frame so that it runs from 0 to 255 and on.
    const int active = activeLines();
    int v = line_;
    if (!pal_) {
        if (active == 192 && line_ > 0xDA) v = line_ - 6;
        else if (active == 224 && line_ > 0xEA) v = line_ - 6;
    } else {
        if (active == 192 && line_ > 0xF2) v = line_ - 57;
        else if (active == 224 && line_ > 0x102) v = line_ - 57;
        else if (active == 240 && line_ > 0x10A) v = line_ - 57;
    }
    return static_cast<uint8_t>(v);
}

uint8_t VdpSms::hcounter(int cycle) const {
    // 342 pixels in 228 cycles, counted in pairs; it skips from 0x93 to 0xE9.
    int h = cycle * 3 / 4;
    if (h > 0x93) h += 0x55;
    return static_cast<uint8_t>(h);
}

bool VdpSms::irq() const { return (frameIrq_ && (regs_[1] & 0x20)) || (lineIrq_ && (regs_[0] & 0x10)); }

uint16_t VdpSms::nameTableBase() const {
    if (activeLines() == 192) return static_cast<uint16_t>((regs_[2] & 0x0E) << 10);
    return static_cast<uint16_t>(((regs_[2] & 0x0C) << 10) | 0x700);
}

void VdpSms::startLine(int line, bool draw) {
    line_ = line;
    const int active = activeLines();
    if (line == 0) {
        vscrollLatch_ = regs_[9];
        frame_.width = 256;
        frame_.wide = 0;
        frame_.height = active;
        frame_.interlaced = false;
    }
    if (line < active) {
        if (draw || (regs_[1] & 0x40)) renderLine(line);  // the sprite flags need the line even unseen
    }
    // The line counter runs on the active lines and the one after them, and is reloaded
    // on every other line.
    if (line <= active) {
        if (lineCounter_ == 0) {
            lineCounter_ = regs_[10];
            lineIrq_ = true;
        } else {
            --lineCounter_;
        }
    } else {
        lineCounter_ = regs_[10];
    }
    if (line == active + 1) frameIrq_ = true;
}

void VdpSms::endFrame() {
    ++frameNumber_;
    frame_.frameNumber = frameNumber_;
}

void VdpSms::renderLine(int line) {
    VideoFrameMd& f = frame_;
    const int o = line * VideoFrameMd::kMaxWidth;
    const bool display = (regs_[1] & 0x40) != 0;
    const int backdropIndex = 16 + (regs_[7] & 0x0F);
    const uint32_t backdrop = colors_[static_cast<size_t>(backdropIndex)];
    f.lines[line].displayEnabled = display;

    if (!display) {
        f.backdrop[line] = backdrop;
        for (int x = 0; x < VideoFrameMd::kMaxWidth; ++x) {
            f.rgba[o + x] = x < 256 ? backdrop : 0;
            f.info[o + x] = MdPixel{static_cast<uint8_t>(backdropIndex), MdLayer::Backdrop, false, MdShade::Normal, 0};
            f.planeA[o + x] = f.planeB[o + x] = f.sprites[o + x] = 0;
            f.planeAInfo[o + x] = f.planeBInfo[o + x] = f.spriteInfo[o + x] = MdPixel{};
        }
        return;
    }

    // Sprites: the first eight on the line are drawn (all of them without the limit);
    // a ninth sets the overflow flag, and two opaque pixels of the first eight meeting
    // set the collision flag. Earlier sprites are in front.
    std::array<uint8_t, 256> spriteColor{};  // 0 = none, else 16..31
    std::array<uint8_t, 256> spriteNumber{};
    {
        const int active = activeLines();
        const uint16_t sat = static_cast<uint16_t>((regs_[5] & 0x7E) << 7);
        const bool tall = (regs_[1] & 0x02) != 0;
        const int zoom = (regs_[1] & 0x01) ? 2 : 1;
        const int height = (tall ? 16 : 8) * zoom;
        int found = 0;
        for (int i = 0; i < 64; ++i) {
            const uint8_t y = vram_[(sat + i) & 0x3FFF];
            if (active == 192 && y == 0xD0) break;
            const int row = (line - y - 1) & 0xFF;
            if (row >= height) continue;
            if (++found > 8) {
                spriteOverflow_ = true;
                if (!unlimitedSprites_) break;
            }
            int x = vram_[(sat + 0x80 + i * 2) & 0x3FFF];
            int n = vram_[(sat + 0x81 + i * 2) & 0x3FFF];
            if (regs_[0] & 0x08) x -= 8;
            if (regs_[6] & 0x04) n |= 0x100;
            if (tall) n &= ~1;
            int r = row / zoom;
            if (r >= 8) {
                ++n;
                r -= 8;
            }
            const int base = n * 32 + r * 4;
            const uint8_t p0 = vram_[base & 0x3FFF], p1 = vram_[(base + 1) & 0x3FFF], p2 = vram_[(base + 2) & 0x3FFF],
                          p3 = vram_[(base + 3) & 0x3FFF];
            for (int px = 0; px < 8; ++px) {
                const int bit = 7 - px;
                const int c = ((p0 >> bit) & 1) | ((p1 >> bit) & 1) << 1 | ((p2 >> bit) & 1) << 2 | ((p3 >> bit) & 1) << 3;
                if (c == 0) continue;
                for (int zx = 0; zx < zoom; ++zx) {
                    const int sx = x + px * zoom + zx;
                    if (sx < 0 || sx >= 256) continue;
                    if (spriteColor[static_cast<size_t>(sx)]) {
                        if (found <= 8) spriteCollision_ = true;
                        continue;
                    }
                    spriteColor[static_cast<size_t>(sx)] = static_cast<uint8_t>(16 + c);
                    spriteNumber[static_cast<size_t>(sx)] = static_cast<uint8_t>(i);
                }
            }
        }
    }

    // Background, mixed with the sprites. Colour 0 of the background is drawn (it is not
    // the backdrop register's colour), but for the 3D view it counts as the sky.
    const int active = activeLines();
    const uint16_t names = nameTableBase();
    const int hscroll = ((regs_[0] & 0x40) && line < 16) ? 0 : regs_[8];
    const bool maskLeft = (regs_[0] & 0x20) != 0;
    f.backdrop[line] = colors_[0];
    f.lines[line].hscrollA = f.lines[line].hscrollB = static_cast<int16_t>(hscroll);
    for (int x = 0; x < 256; ++x) {
        const int i = o + x;
        if (maskLeft && x < 8) {
            f.rgba[i] = backdrop;
            f.info[i] = MdPixel{static_cast<uint8_t>(backdropIndex), MdLayer::Backdrop, false, MdShade::Normal, 0};
            f.planeA[i] = f.planeB[i] = f.sprites[i] = 0;
            f.planeAInfo[i] = f.planeBInfo[i] = f.spriteInfo[i] = MdPixel{};
            continue;
        }
        const int vscroll = ((regs_[0] & 0x80) && x >= 192) ? 0 : vscrollLatch_;
        const int y = active == 192 ? (line + vscroll) % 224 : (line + vscroll) & 0xFF;
        const int sx = (x - hscroll) & 0xFF;
        const uint16_t entryAt = static_cast<uint16_t>((names + ((y >> 3) * 32 + (sx >> 3)) * 2) & 0x3FFF);
        const uint16_t entry = static_cast<uint16_t>(vram_[entryAt] | vram_[(entryAt + 1) & 0x3FFF] << 8);
        const uint16_t tile = entry & 0x1FF;
        int py = y & 7, px = sx & 7;
        if (entry & 0x0400) py = 7 - py;
        if (entry & 0x0200) px = 7 - px;
        const int base = tile * 32 + py * 4;
        const int bit = 7 - px;
        const int index = ((vram_[base & 0x3FFF] >> bit) & 1) | ((vram_[(base + 1) & 0x3FFF] >> bit) & 1) << 1 |
                          ((vram_[(base + 2) & 0x3FFF] >> bit) & 1) << 2 | ((vram_[(base + 3) & 0x3FFF] >> bit) & 1) << 3;
        const uint8_t bgColor = static_cast<uint8_t>(index + ((entry & 0x0800) ? 16 : 0));
        const bool priority = (entry & 0x1000) != 0;
        const bool front = priority && index != 0;  // in front of the sprites
        const uint8_t s = spriteColor[static_cast<size_t>(x)];

        const MdLayer bgLayer = index == 0 ? MdLayer::Backdrop : front ? MdLayer::PlaneA : MdLayer::PlaneB;
        const MdPixel bgPixel{bgColor, bgLayer, priority, MdShade::Normal, tile};
        if (s && !front) {
            f.rgba[i] = colors_[s];
            f.info[i] = MdPixel{s, MdLayer::Sprite, false, MdShade::Normal, spriteNumber[static_cast<size_t>(x)]};
        } else {
            f.rgba[i] = colors_[bgColor];
            f.info[i] = bgPixel;
        }
        f.planeB[i] = (index != 0 && !front) ? colors_[bgColor] : 0;
        f.planeBInfo[i] = MdPixel{bgColor, MdLayer::PlaneB, priority, MdShade::Normal, tile};
        f.planeA[i] = front ? colors_[bgColor] : 0;
        f.planeAInfo[i] = front ? bgPixel : MdPixel{};
        f.sprites[i] = s ? colors_[s] : 0;
        f.spriteInfo[i] = MdPixel{s, MdLayer::Sprite, false, MdShade::Normal, spriteNumber[static_cast<size_t>(x)]};
    }
    for (int x = 256; x < VideoFrameMd::kMaxWidth; ++x) {
        f.rgba[o + x] = f.planeA[o + x] = f.planeB[o + x] = f.sprites[o + x] = 0;
        f.info[o + x] = f.planeAInfo[o + x] = f.planeBInfo[o + x] = f.spriteInfo[o + x] = MdPixel{};
    }
}

}  // namespace plumsg
