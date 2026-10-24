#include "paprium.h"

#include <algorithm>
#include <cstring>

namespace plumsg {

namespace {

// Where things are in the chip's RAM.
constexpr uint32_t kArgs = 0x1E10;      // a command's arguments, a word each
constexpr uint32_t kCommand = 0x1FEA;   // high byte: the command, low byte: its argument
constexpr uint32_t kStream = 0xC000;    // unpacked data, read out a block at a time
constexpr uint32_t kObjects = 0x0F80;   // 0x31 objects of 16 bytes: what to draw
constexpr uint32_t kSprites = 0x0B00;   // the sprite table the chip builds, 80 entries
constexpr uint32_t kSpritesShown = 0x1F20;  // the 14 entries the game's second pass shows
constexpr uint32_t kDmaList = 0x1400;   // VDP register writes for each DMA, 16 bytes each
constexpr uint32_t kDmaCount = 0x1F16;
constexpr uint32_t kSpriteCount = 0x1F18;
constexpr int kSaveBank = 0x780;

}  // namespace

bool Paprium::detect(const Cartridge& cart) {
    const auto& rom = cart.rom();
    if (rom.size() < 0x200) return false;
    return std::memcmp(&rom[0x180], "GM T-574120", 11) == 0;
}

void Paprium::reset() {
    const auto& rom = cart_.rom();
    ram_.fill(0);
    std::copy_n(rom.begin(), std::min<size_t>(rom.size(), ram_.size()), ram_.begin());
    // The chip changes what it copied before the 68000 starts. The ROM's start-up code
    // is a decoy for copies without the chip: a "PAAAAAPRIUM - Vapor Ware 2044" title.
    // The chip makes its emulator check pass, sends it to the game at 0x10100, and says
    // which pads may be plugged in (6 buttons, multitap).
    ram_[0x1D1D] = 0x04;  // movea.l #$C00004,a0: the check reads the VDP status
    ram_[0x1D2C] = 0x67;  // beq: and passes
    static const uint8_t kGame[6] = {0x4E, 0xF9, 0x00, 0x01, 0x01, 0x00};  // jmp $10100
    std::copy_n(kGame, 6, ram_.begin() + 0x1560);
    setWord(0x192, 0x3634);
    unpacked_.fill(0);
    scaler_.fill(0);
    extraSprites_.fill(0);
    frame_.fill(0);
    streamMode_ = 0;
    streamPos_ = streamLeft_ = 0;
    tileSrc_ = tileDst_ = 0;
    voices_.fill(Voice{});
    echoL_.fill(0);
    echoR_.fill(0);
    echoPos_ = 0;
    echoSide_ = 0;
    sfxVolume_ = 0;
    audioFlags_ = 0;
    audioPhase_ = 0;
    samples_.clear();
    sfxPtr_ = romLong(romLong(0xAF77C) + 0x778);
    // Where the boot command will say the data is; a state loaded needs it too.
    spritePtr_ = romLong(0x10014);
    tilePtr_ = romLong(romLong(0xAF77C) + 0x780);
    loadObjects();
}

uint8_t Paprium::read8(uint32_t addr) {
    addr &= 0xFFFF;
    return ram_[addr];
}

uint16_t Paprium::read16(uint32_t addr) {
    addr &= 0xFFFE;
    if (addr == kStream && streamLeft_ > 0) {
        // Each read of the first word brings in the next block.
        const uint32_t block = streamMode_ == 2 ? 0x4000 : streamMode_ == 7 ? 0x800 : 0;
        const uint32_t n = std::min({block, streamLeft_, static_cast<uint32_t>(unpacked_.size()) - streamPos_});
        std::memcpy(&ram_[kStream], &unpacked_[streamPos_], n);
        streamPos_ += n;
        streamLeft_ -= n;
    }
    switch (addr) {
        case 0x1FE4: return 0xFFBB;
        case 0x1FE6: return 0xBCFF;  // bits 8 and 9: the save memory is there
        case kCommand: return 0x7FFF;  // bit 15 clear: ready for a command
        default: return word(addr);
    }
}

void Paprium::write8(uint32_t addr, uint8_t value) { ram_[addr & 0xFFFF] = value; }

void Paprium::write16(uint32_t addr, uint16_t value) {
    addr &= 0xFFFE;
    setWord(addr, value);
    if (addr == kCommand) {
        command(value);
        setWord(kCommand, word(kCommand) & 0x7FFF);
    }
}

void Paprium::command(uint16_t value) {
    const int arg = value & 0xFF;
    switch (value >> 8) {
        case 0x88: audioFlags_ = static_cast<uint8_t>(arg); break;
        case 0xCA: sfxVolume_ = arg; break;
        case 0xD1: playSound(arg); break;
        case 0xD2: stopSound(arg); break;
        case 0xD3: loopSound(arg); break;
        case 0x84:  // the ROM's second 32 KB, back over the RAM
            std::copy_n(cart_.rom().begin() + 0x8000, 0x8000, ram_.begin() + 0x8000);
            break;
        case 0xAD: drawObject(arg); break;
        case 0xAE: startSprites(); break;
        case 0xAF: endSprites(arg); break;
        case 0xB0: std::fill_n(ram_.begin() + kSpritesShown, 14 * 8, 0); break;
        case 0xB1: pauseSprites(); break;
        case 0xC6:  // boot: where the data is
            spritePtr_ = arg32(kArgs + 0x14);
            tilePtr_ = arg32(kArgs + 0x18);
            sfxPtr_ = arg32(kArgs + 0x10);
            loadObjects();
            streamLeft_ = 0;
            break;
        case 0xDA: {  // unpack a block, to be read out at 0xC000
            const uint32_t offset = word(kArgs);
            std::vector<uint8_t> out;
            const size_t size = unpack(arg32(kArgs + 2), out);
            const size_t n = offset < unpacked_.size() ? std::min(size, unpacked_.size() - offset) : 0;
            std::copy_n(out.begin(), n, unpacked_.begin() + offset);
            streamMode_ = static_cast<uint8_t>(arg);
            streamPos_ = offset;
            streamLeft_ = static_cast<uint32_t>(n);
            break;
        }
        case 0xDB:  // read out again part of what was unpacked
            streamPos_ = std::min<uint32_t>(word(kArgs + 2), unpacked_.size());
            streamLeft_ = word(kArgs + 4);
            break;
        case 0xDF: saveRead(arg); break;
        case 0xE0: saveWrite(arg); break;
        case 0xF4: scalerLoad(); break;
        case 0xF5: scale(); break;
        default: break;  // the music and the rest: not emulated
    }
}

// Two packings, told apart by the first byte: 0x80 a run-length/LZ one with a byte of
// distance, 0x81 an LZO-like one.
size_t Paprium::unpack(uint32_t src, std::vector<uint8_t>& out) const {
    out.clear();
    const uint8_t type = romByte(src++);
    auto copyBack = [&](uint32_t distance, int len) {
        size_t from = out.size() - std::min<size_t>(distance, out.size());
        for (int i = 0; i < len; ++i) out.push_back(out[from++]);
    };
    if (type == 0x80) {
        for (;;) {
            const uint8_t code = romByte(src++);
            const int kind = code >> 6, len = code & 0x3F;
            if (code == 0) break;
            if (kind == 0) {
                for (int i = 0; i < len; ++i) out.push_back(romByte(src++));
            } else if (kind == 1) {
                out.insert(out.end(), len, romByte(src++));
            } else if (kind == 2) {
                copyBack(romByte(src++), len);
            } else {
                out.insert(out.end(), len, 0);
            }
            if (out.size() > 0x100000) break;
        }
    } else if (type == 0x81) {
        // A long length: zero bytes add 255 each, then the first non-zero byte.
        auto longLength = [&](int base) {
            int extra = 0;
            uint8_t b;
            while ((b = romByte(src++)) == 0) extra += 255;
            return b + extra + base;
        };
        int state = 0;  // literals that followed the last match (4: a run of them)
        for (;;) {
            const uint8_t code = romByte(src++);
            int len = 0, raw = 0;
            uint32_t distance = 0;
            if (code & 0x80) {
                raw = code & 3;
                len = ((code >> 5) & 3) + 5;
                distance = ((code >> 2) & 7) + (romByte(src++) << 3) + 1;
            } else if (code & 0x40) {
                raw = code & 3;
                len = ((code >> 5) & 1) + 3;
                distance = ((code >> 2) & 7) + (romByte(src++) << 3) + 1;
            } else if (code & 0x20) {
                len = code & 0x1F;
                if (len == 0) len = longLength(31);
                len += 2;
                const uint8_t b = romByte(src++);
                raw = b & 3;
                distance = (b >> 2) + (romByte(src++) << 6) + 1;
            } else if (code & 0x10) {
                len = code & 7;
                if (len == 0) len = longLength(7);
                len += 2;
                distance = ((code >> 3) & 1) << 14;
                const uint8_t b = romByte(src++);
                raw = b & 3;
                distance += (b >> 2) + (romByte(src++) << 6);
                if (distance == 0) break;  // the end
                distance += 16384;
            } else if (state == 0) {
                raw = code & 0x0F;
                if (raw == 0) raw = longLength(15);
                raw += 3;
            } else {
                raw = code & 3;
                distance = ((code >> 2) & 3) + (romByte(src++) << 2) + (state < 4 ? 1 : 2049);
                len = state < 4 ? 2 : 3;
            }
            state = len > 0 ? raw : 4;
            if (len > 0) copyBack(distance, len);
            for (int i = 0; i < raw; ++i) out.push_back(romByte(src++));
            if (out.size() > 0x100000) break;
        }
    }
    return out.size();
}

void Paprium::loadObjects() { unpack(spritePtr_, objects_); }

// The chip's own tables keep 32-bit values as two words, the low one first.
static uint32_t pairAt(const std::vector<uint8_t>& v, uint32_t at) {
    auto w = [&](uint32_t a) -> uint32_t { return a + 1 < v.size() ? (v[a] << 8 | v[a + 1]) : 0; };
    return w(at) | w(at + 2) << 16;
}

// Draws object `index` (its entry at 0x0F80): steps its animation, adds its pieces to
// the sprite table and their tiles to the DMA list.
void Paprium::drawObject(int index) {
    if (index < 0 || index >= static_cast<int>(frame_.size())) return;
    const uint32_t entry = kObjects + static_cast<uint32_t>(index) * 16;
    int anim = word(entry);
    const int nextAnim = word(entry + 2);
    const int obj = word(entry + 4) & 0x7FFF;
    const int objAttr = word(entry + 8);
    int reset = word(entry + 10);
    const int posX = word(entry + 12), posY = word(entry + 14);
    const bool flip = (objAttr & 0x800) != 0;
    const uint32_t animTable = pairAt(objects_, static_cast<uint32_t>(obj + 1) * 4);
    uint32_t frame = frame_[index];
    uint32_t pieces = 0;
    for (;;) {
        if (reset == 1) {
            frame = pairAt(objects_, animTable + static_cast<uint32_t>(anim) * 4);
            setWord(entry + 10, 0);
        }
        if (frame == 0 || frame == 0xFFFFFFFF) {
            setWord(entry, 0);
            return;
        }
        const uint32_t v = pairAt(objects_, frame);
        pieces = v & 0xFFFFFF;
        const uint8_t flags = static_cast<uint8_t>(v >> 24);
        if (pieces == 0) return;
        frame += 4;
        if (flags < 0x80) {
            if (nextAnim == 0xFFFF) {
                frame = pairAt(objects_, frame) & 0xFFFFFF;  // loops on
            } else if (reset == 0) {
                anim = nextAnim;
                reset = 1;
                continue;
            }
        }
        break;
    }
    frame_[index] = frame;

    auto obyte = [&](uint32_t a) -> uint8_t { return a < objects_.size() ? objects_[a] : 0; };
    auto oword = [&](uint32_t a) -> int { return obyte(a) << 8 | obyte(a + 1); };
    const int count = obyte(pieces);
    pieces += 2;
    int dmaCount = word(kDmaCount);
    int spriteCount = word(kSpriteCount);
    int x = posX, y = posY;
    std::vector<uint8_t> tiles;
    for (int i = 0; i < count; ++i, pieces += 8) {
        const int sizeX = ((obyte(pieces + 2) >> 2) & 3) + 1;
        const int sizeY = (obyte(pieces + 2) & 3) + 1;
        const int tile = oword(pieces + 4);
        const int tileAttr = oword(pieces + 6) & ~0x1FF;
        const int offset = oword(pieces + 6) & 0x1FF;
        x += flip ? -static_cast<int8_t>(obyte(pieces)) : static_cast<int8_t>(obyte(pieces));
        y += static_cast<int8_t>(obyte(pieces + 1));
        if (tile == 0 || spriteCount >= 94) continue;
        const int bytes = sizeX * sizeY * 0x20;
        const int attr = ((tileAttr & 0x8000) ? 0x8000 : (objAttr & 0x8000)) +
                         ((tileAttr & 0x6000) ? (tileAttr & 0x6000) : (objAttr & 0x6000)) +
                         ((tileAttr & 0x1800) ^ (objAttr & 0x1800));
        const bool onScreen = y >= 128 && y + sizeY * 8 < 368 &&
                              (flip ? (x - sizeX * 8 < 448 && x >= 128) : (x + sizeX * 8 >= 128 && x < 448));
        if (onScreen) {
            uint8_t* at;
            int link;
            if (spriteCount < 80) {
                at = &ram_[kSprites + spriteCount * 8];
                link = spriteCount + 1;
            } else {
                if (spriteCount == 80) {  // the first entry heads the second list too
                    std::memcpy(extraSprites_.data(), &ram_[kSprites], 8);
                    extraSprites_[3] = 14;
                    ram_[0xD7B] = 1;
                    ++spriteCount;
                }
                at = &extraSprites_[(spriteCount - 80) * 8];
                link = spriteCount - 80 + 1;
            }
            const int sx = (x - (flip ? sizeX * 8 : 0)) & 0x1FF;
            const uint16_t e[4] = {static_cast<uint16_t>(y & 0x3FF),
                                   static_cast<uint16_t>((sizeX - 1) << 10 | (sizeY - 1) << 8 | link),
                                   static_cast<uint16_t>(attr + ((tileDst_ / 0x20) & 0x7FF)), static_cast<uint16_t>(sx)};
            for (int k = 0; k < 4; ++k) {
                at[k * 2] = static_cast<uint8_t>(e[k] >> 8);
                at[k * 2 + 1] = static_cast<uint8_t>(e[k]);
            }
            ++spriteCount;
        }
        // The piece's tiles go to RAM, and a DMA takes them to VRAM.
        const uint32_t tileRef = tilePtr_ + static_cast<uint32_t>(tile) * 4;
        unpack(tilePtr_ + romLong(tileRef), tiles);
        for (int b = 0; b < bytes; ++b) {
            const size_t from = static_cast<size_t>(offset) * 0x20 + b;
            ram_[(tileSrc_ + b) & 0xFFFF] = from < tiles.size() ? tiles[from] : 0;
        }
        const uint32_t d = kDmaList + static_cast<uint32_t>(dmaCount) * 16;
        const uint16_t regs[8] = {0x8F02,
                                  static_cast<uint16_t>(0x9300 | ((bytes >> 1) & 0xFF)),
                                  static_cast<uint16_t>(0x9500 | ((tileSrc_ >> 1) & 0xFF)),
                                  static_cast<uint16_t>(0x9400 | ((bytes >> 9) & 0xFF)),
                                  0x9700,
                                  static_cast<uint16_t>(0x9600 | ((tileSrc_ >> 9) & 0xFF)),
                                  static_cast<uint16_t>(0x4000 | (tileDst_ & 0x3FFF)),
                                  static_cast<uint16_t>(0x0080 | (tileDst_ >> 14))};
        for (int k = 0; k < 8; ++k) setWord(d + k * 2, regs[k]);
        setWord(kDmaCount, static_cast<uint16_t>(++dmaCount));
        setWord(kSpriteCount, static_cast<uint16_t>(spriteCount));
        tileSrc_ += bytes;
        tileDst_ += bytes;
    }
}

// A new frame of sprites: the DMA list starts with the sprite table itself.
void Paprium::startSprites() {
    const int count = word(kSpriteCount);
    setWord(kDmaCount, 1);
    static const uint16_t kTable[8] = {0x8F02, 0x9340, 0x9580, 0x9401, 0x9700, 0x9605, 0x7000, 0x0083};
    for (int k = 0; k < 8; ++k) setWord(kDmaList + k * 2, kTable[k]);
    if (count < 80) std::memcpy(&ram_[kSpritesShown], &ram_[kSprites], 14 * 8);
    else std::memcpy(&ram_[kSpritesShown], extraSprites_.data(), 14 * 8);
    tileSrc_ = 0x2000;
    tileDst_ = 0x200;
}

// The last sprite of the frame ends the table's links.
void Paprium::endSprites(int arg) {
    const int count = word(kSpriteCount);
    if (count == 0) {
        std::fill_n(ram_.begin() + kSprites, 8, 0);
        std::fill_n(extraSprites_.begin(), 8, 0);
    } else if (count <= 80) {
        ram_[kSprites + (count - 1) * 8 + 3] = 0;
        if (count <= 14) extraSprites_[(count - 1) * 8 + 3] = 0;
    } else {
        extraSprites_[(count - 81) * 8 + 3] = 0;
    }
    if (arg == 2) setWord(0x1F1C, 1);
}

void Paprium::pauseSprites() {
    if (word(kSpriteCount) == 0) std::fill_n(ram_.begin() + kSprites, 8, 0);
}

// The scaler's picture: 64x64 pixels, 4 bits each, unpacked one pixel a byte by columns.
void Paprium::scalerLoad() {
    std::vector<uint8_t> pic;
    unpack(arg32(kArgs), pic);
    pic.resize(0x800, 0);
    size_t out = 0;
    for (int col = 0; col < 64; ++col)
        for (int row = 0; row < 64; ++row) {
            const uint8_t b = pic[row * 32 + col / 2];
            scaler_[out++] = (col & 1) ? (b & 0x0F) : (b >> 4);
        }
}

// Draws the picture squeezed between columns `left` and `right` into the two tile
// blocks at 0x200 and 0x600.
void Paprium::scale() {
    const int left = word(kArgs), right = word(kArgs + 2);
    const int scaleBy = std::max<int>(word(kArgs + 4), 1);
    int src = word(kArgs + 6);
    const int step = 64 * 0x10000 / scaleBy;
    int frac = 0;
    for (int col = 0; col < 128; ++col) {
        const uint32_t base = (col & 4) ? 0x600 : 0x200;
        uint32_t out = static_cast<uint32_t>((col / 8) * 64 + ((col & 2) / 2));
        const bool inside = col >= left && col < right;
        for (int row = 0; row < 64; row += 2, out += 2) {
            uint8_t& b = ram_[(base + out) & 0xFFFF];
            const uint8_t px = scaler_[(row * 64 + src) & 0xFFF];
            if (inside) b = (col & 1) ? static_cast<uint8_t>(b + (px & 0x0F)) : static_cast<uint8_t>(px << 4);
            else if (!(col & 1)) b = 0;
        }
        if (inside && src < 64) {
            frac += 0x10000;
            while (frac >= step) {
                ++src;
                frac -= step;
            }
        }
    }
}

void Paprium::saveRead(int bank) {
    if (bank < 1 || bank > 4) return;
    const uint32_t to = word(kArgs);
    for (int i = 0; i < kSaveBank; ++i)
        ram_[(to + i) & 0xFFFF] = cart_.saveRamByte(static_cast<uint32_t>((bank - 1) * kSaveBank + i));
}

void Paprium::saveWrite(int bank) {
    if (bank < 1 || bank > 4) return;
    const uint32_t from = word(kArgs + 2);
    for (int i = 0; i < kSaveBank; ++i)
        cart_.writeSaveRamByte(static_cast<uint32_t>((bank - 1) * kSaveBank + i), ram_[(from + i) & 0xFFFF]);
}

// ---------------------------------------------------------------------------------
// Sound effects. Each has 8 bytes at sfxPtr_: where its samples are (from sfxPtr_), its
// type, and how many samples it has (24 bits).

void Paprium::playSound(int number) {
    const uint32_t entry = sfxPtr_ + static_cast<uint32_t>(number) * 8;
    int mask = word(kArgs);
    // The first free voice of those allowed; with none free, the one playing longest.
    int pick = 0;
    uint32_t oldest = 0;
    for (int i = 0; i < 8; ++i, mask >>= 1) {
        if (!(mask & 1)) continue;
        if (voices_[i].left == 0) {
            pick = i;
            break;
        }
        if (voices_[i].age > oldest) {
            oldest = voices_[i].age;
            pick = i;
        }
    }
    Voice& v = voices_[pick];
    v = Voice{};
    v.volume = word(kArgs + 2);
    v.pan = word(kArgs + 4);
    v.flags = word(kArgs + 6);
    v.number = static_cast<uint16_t>(number);
    v.type = romByte(entry + 4);
    if (v.flags & 0x4000) v.echoLeft = (echoSide_++ & 1) != 0;
    v.ptr = romLong(entry);
    v.left = static_cast<uint32_t>(romByte(entry + 5)) << 16 | romByte(entry + 6) << 8 | romByte(entry + 7);
}

void Paprium::loopSound(int mask) {
    for (int i = 0; i < 8; ++i, mask >>= 1) {
        if (!(mask & 1)) continue;
        voices_[i].volume = word(kArgs);
        voices_[i].pan = word(kArgs + 2);
        voices_[i].loop = true;
        break;
    }
}

void Paprium::stopSound(int mask) {
    for (int i = 0; i < 8; ++i) {
        if (!(mask & (1 << i))) continue;
        if (word(kArgs) == 0) voices_[i].left = 0;  // otherwise it fades: not emulated
        voices_[i].loop = false;
        break;
    }
}

// One 48 kHz sample of all the voices, with the echo.
void Paprium::soundSample(int& l, int& r) {
    static const int kSteps[6] = {1, 2, 4, 5, 8, 9};  // 48 kHz divided by these
    for (Voice& v : voices_) {
        if (v.left > 0) {
            const int depth = v.type & 3;
            const int32_t step = kSteps[std::min(v.type >> 4, 5)] << 16;
            int s = romByte(sfxPtr_ + v.ptr);
            if (depth == 1) s = s * 256 - 32768;
            else if (depth == 2) s = ((v.phase == 0 ? s >> 4 : s) & 0x0F) * 4096 - 32768;
            s = s * v.volume / 0x400;
            const int sl = s * (v.pan <= 0x80 ? 0x80 : 0x100 - v.pan) / 0x80;
            const int sr = s * (v.pan >= 0x80 ? 0x80 : v.pan) / 0x80;
            l += sl;
            r += sr;
            if (v.flags & 0x4000) {
                if (v.echoLeft) echoL_[echoPos_] += sl * 33 / 100;
                else echoR_[echoPos_] += sr * 33 / 100;
            }
            if (v.flags & 0x100) {  // louder: everything mixed so far
                l = l * 125 / 100;
                r = r * 125 / 100;
            }
            ++v.age;
            v.tick += 0x10000 - ((v.flags & 0x8000) ? 0x800 : 0) - ((v.flags & 0x2000) ? 0x8000 : 0);
            if (v.tick >= step) {
                v.tick -= step;
                --v.left;
                if (++v.phase >= depth) {
                    ++v.ptr;
                    v.phase = 0;
                }
            }
        }
        if (v.left == 0) {
            v.phase = 0;
            if (v.loop) {
                const uint32_t entry = sfxPtr_ + v.number * 8u;
                v.ptr = romLong(entry);
                v.left = static_cast<uint32_t>(romByte(entry + 5)) << 16 | romByte(entry + 6) << 8 | romByte(entry + 7);
            }
        }
    }
}

void Paprium::runAudio(size_t count, double rate) {
    const double step = 48000.0 / rate;
    for (size_t n = 0; n < count; ++n) {
        int l = 0, r = 0;
        audioPhase_ += step;
        // Keeps the last 48 kHz sample of those this output sample covers.
        while (audioPhase_ >= 1.0) {
            audioPhase_ -= 1.0;
            echoL_[echoPos_] = echoR_[echoPos_] = 0;
            l = r = 0;
            soundSample(l, r);
            echoPos_ = (echoPos_ + 1) % kEcho;
            l += echoL_[echoPos_];
            r += echoR_[echoPos_];
            l = l * sfxVolume_ / 0x100;
            r = r * sfxVolume_ / 0x100;
            if (audioFlags_ & 0x08) {
                l *= 2;
                r *= 2;
            }
        }
        samples_.push_back(static_cast<int16_t>(std::clamp(l, -32768, 32767)));
        samples_.push_back(static_cast<int16_t>(std::clamp(r, -32768, 32767)));
    }
}

}  // namespace plumsg
