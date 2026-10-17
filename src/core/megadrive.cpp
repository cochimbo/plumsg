#include "megadrive.h"

#include <algorithm>
#include <cstring>

#include "miniz.h"
#include "state_io.h"

namespace plumsg {

namespace {

constexpr uint32_t kStateMagic = 0x53444D50;  // "PMDS"
constexpr uint32_t kStateVersion = 1;
constexpr size_t kMaxStateSize = 4 << 20;

constexpr uint32_t fourcc(const char (&s)[5]) {
    return static_cast<uint32_t>(s[0]) | static_cast<uint32_t>(s[1]) << 8 | static_cast<uint32_t>(s[2]) << 16 |
           static_cast<uint32_t>(s[3]) << 24;
}

}  // namespace

MegaDrive::MegaDrive() {
    vdp_.setDmaReader([this](uint32_t addr) { return read16(addr & 0xFFFFFE); });
    applyRegion();
    powerCycle();
}

bool MegaDrive::loadRom(const std::string& path, std::string* error) {
    Cartridge loaded;
    if (!loaded.loadFile(path, error)) return false;
    cart_ = std::move(loaded);
    applyRegion();
    powerCycle();
    return true;
}

void MegaDrive::loadRomData(std::vector<uint8_t> rom) {
    cart_.loadData(std::move(rom));
    applyRegion();
    powerCycle();
}

void MegaDrive::setRegion(Region r) {
    region_ = r;
    applyRegion();
}

void MegaDrive::applyRegion() {
    configureFor(region_ == Region::Auto ? (cart_.loaded() ? cart_.preferredRegion() : Region::UsaNtsc) : region_);
}

void MegaDrive::configureFor(Region r) {
    effectiveRegion_ = r;
    const bool isPal = effectiveRegion_ == Region::EuropePal;
    vdp_.setPal(isPal);
    psg_.setClock(isPal ? Psg::kClockPal : Psg::kClockNtsc);
    const double master = isPal ? kMasterPal : kMasterNtsc;
    ym_.setMasterClock(master);
    mixer_.setFmRate(ym_.sampleRate());
    // Version register: bit 7 overseas, bit 6 PAL, bit 5 no expansion unit; version 0
    // (no TMSS).
    uint8_t version = 0x20;
    if (effectiveRegion_ != Region::JapanNtsc) version |= 0x80;
    if (isPal) version |= 0x40;
    io_.setVersion(version);
}

void MegaDrive::setSampleRate(double hz) {
    psg_.setSampleRate(hz);
    mixer_.setOutputRate(hz);
}

void MegaDrive::powerCycle() {
    ram_.fill(0);
    zram_.fill(0);
    vdp_.reset();
    reset();
}

void MegaDrive::reset() {
    cart_.reset();
    loadedThumbnail_.clear();
    io_.reset();
    ym_.reset();
    psg_.reset();
    psg_.setStereo(0xFF);
    mixer_.reset();
    z80_.reset();
    z80Bank_ = 0;
    z80BusReq_ = false;
    z80Reset_ = true;
    now_ = lineStart_ = m68kClock_ = z80Clock_ = psgClock_ = 0;
    psgRemainder_ = 0;
    ym_.setClockPosition(0);
    psgBuffer_.clear();
    cpu_.setIrqLevel(0);
    cpu_.reset();
}

// ---------------------------------------------------------------------------------
// Scheduler

void MegaDrive::runUntil(uint64_t clock) {
    while (m68kClock_ < clock || z80Clock_ < clock) {
        // Run whichever CPU is further behind, one instruction at a time.
        if (m68kClock_ < clock && (m68kClock_ <= z80Clock_ || z80Clock_ >= clock)) {
            now_ = m68kClock_;
            m68kClock_ += static_cast<uint64_t>(cpu_.step()) * 7;
        } else if (z80Running()) {
            now_ = z80Clock_;
            z80Clock_ += static_cast<uint64_t>(z80_.step()) * 15;
        } else {
            z80Clock_ = clock;
        }
    }
    now_ = clock;
}

void MegaDrive::syncSound() {
    if (now_ > psgClock_) {
        const uint64_t clocks = now_ - psgClock_ + static_cast<uint64_t>(psgRemainder_);
        psg_.run(static_cast<int>(clocks / 15));
        psgRemainder_ = static_cast<int>(clocks % 15);
        psgClock_ = now_;
    }
    ym_.runUntil(now_);
}

void MegaDrive::runFrame() {
    const int lines = vdp_.linesPerFrame();
    for (int line = 0; line < lines; ++line) {
        lineStart_ = now_;
        vdp_.startLine(line);
        updateIrq();
        z80_.setIrqLine(vdp_.z80Irq());
        runUntil(lineStart_ + VdpMd::kActiveClocks);
        if (line < vdp_.activeLines()) vdp_.renderLine(line);
        vdp_.endLine(line);
        updateIrq();
        runUntil(lineStart_ + kMasterPerLine);
    }
    syncSound();
    vdp_.endFrame();
    ++frames_;
    loadedThumbnail_.clear();
    // Hand this frame's sound to the mixer.
    psgBuffer_.insert(psgBuffer_.end(), psg_.samples().begin(), psg_.samples().end());
    psg_.clearSamples();
    mixer_.mix(ym_.samples(), psgBuffer_);
}

void MegaDrive::irqAcknowledge(int level) {
    vdp_.acknowledge(level);
    updateIrq();
}

// ---------------------------------------------------------------------------------
// 68000 memory map

uint16_t MegaDrive::readVdp(uint32_t addr) {
    switch (addr & 0x1F) {
        case 0x00: case 0x02: return vdp_.readData();
        case 0x04: case 0x06: return vdp_.readStatus(lineClock());
        case 0x08: case 0x0A: case 0x0C: case 0x0E: return vdp_.readHV(lineClock());
        default: return 0xFFFF;
    }
}

void MegaDrive::writeVdp(uint32_t addr, uint16_t value, bool byte) {
    const uint32_t port = addr & 0x1F;
    if (port >= 0x10 && port < 0x18) {
        if (!byte || (addr & 1)) {
            syncSound();
            psg_.write(static_cast<uint8_t>(value));
        }
        return;
    }
    // Byte writes reach the VDP with the byte on both halves of the bus.
    if (byte) value = static_cast<uint16_t>((value & 0xFF) << 8 | (value & 0xFF));
    int stall = 0;
    if (port < 0x04) stall = vdp_.writeData(value);
    else if (port < 0x08) stall = vdp_.writeControl(value);
    m68kClock_ += static_cast<uint64_t>(stall);
    updateIrq();
}

uint8_t MegaDrive::readZ80Area(uint32_t addr) {
    if (!z80BusFree()) return 0xFF;
    return read(static_cast<uint16_t>(addr & 0x7FFF));
}

void MegaDrive::writeZ80Area(uint32_t addr, uint8_t value) {
    if (!z80BusFree()) return;
    write(static_cast<uint16_t>(addr & 0x7FFF), value);
}

void MegaDrive::writeBusControl(uint32_t addr, bool set) {
    if ((addr & 0xFFFF00) == 0xA11100) {
        if (set && !z80BusReq_) z80Clock_ = std::max(z80Clock_, now_);
        if (!set && z80BusReq_) z80Clock_ = std::max(z80Clock_, now_);
        z80BusReq_ = set;
    } else if ((addr & 0xFFFF00) == 0xA11200) {
        const bool reset = !set;
        if (reset && !z80Reset_) {
            z80_.reset();
            syncSound();
            ym_.reset();
        }
        if (!reset && z80Reset_) z80Clock_ = std::max(z80Clock_, now_);
        z80Reset_ = reset;
    }
}

uint8_t MegaDrive::read8(uint32_t addr) {
    addr &= 0xFFFFFF;
    if (addr < 0x400000) return cart_.read8(addr);
    if (addr >= 0xE00000) return ram_[addr & 0xFFFF];
    if (addr >= 0xC00000) {
        if (addr >= 0xC00020) return 0xFF;
        const uint16_t w = readVdp(addr & ~1u);
        return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
    if (addr < 0xA10000 && addr >= 0xA00000) return readZ80Area(addr);
    if (addr >= 0xA10000 && addr < 0xA10020) return io_.read(addr | 1, now_);
    if (addr == 0xA11100) return z80BusReq_ && !z80Reset_ ? 0x00 : 0x01;
    return 0x00;
}

uint16_t MegaDrive::read16(uint32_t addr) {
    addr &= 0xFFFFFE;
    if (addr < 0x400000) return cart_.read16(addr);
    if (addr >= 0xE00000) return static_cast<uint16_t>(ram_[addr & 0xFFFF] << 8 | ram_[(addr & 0xFFFF) + 1]);
    if (addr >= 0xC00000) return addr < 0xC00020 ? readVdp(addr) : 0xFFFF;
    if (addr < 0xA10000 && addr >= 0xA00000) {
        const uint8_t b = readZ80Area(addr);
        return static_cast<uint16_t>(b << 8 | b);
    }
    if (addr >= 0xA10000 && addr < 0xA10020) {
        const uint8_t b = io_.read(addr | 1, now_);
        return static_cast<uint16_t>(b << 8 | b);
    }
    if (addr == 0xA11100) return z80BusReq_ && !z80Reset_ ? 0x0000 : 0x0100;
    return 0x0000;
}

void MegaDrive::write8(uint32_t addr, uint8_t value) {
    addr &= 0xFFFFFF;
    if (addr >= 0xE00000) {
        ram_[addr & 0xFFFF] = value;
    } else if (addr < 0x400000) {
        cart_.write8(addr, value);
    } else if (addr >= 0xC00000) {
        if (addr < 0xC00020) writeVdp(addr, value, true);
    } else if (addr >= 0xA00000 && addr < 0xA10000) {
        writeZ80Area(addr, value);
    } else if (addr >= 0xA10000 && addr < 0xA10020) {
        io_.write(addr | 1, value, now_);
    } else if (addr == 0xA11100 || addr == 0xA11200) {
        writeBusControl(addr, (value & 1) != 0);
    } else if (addr >= 0xA130F0 && addr < 0xA13100) {
        cart_.writeTimeRegister(addr, value);
    }
}

void MegaDrive::write16(uint32_t addr, uint16_t value) {
    addr &= 0xFFFFFE;
    if (addr >= 0xE00000) {
        ram_[addr & 0xFFFF] = static_cast<uint8_t>(value >> 8);
        ram_[(addr & 0xFFFF) + 1] = static_cast<uint8_t>(value);
    } else if (addr < 0x400000) {
        cart_.write16(addr, value);
    } else if (addr >= 0xC00000) {
        if (addr < 0xC00020) writeVdp(addr, value, false);
    } else if (addr >= 0xA00000 && addr < 0xA10000) {
        writeZ80Area(addr, static_cast<uint8_t>(value >> 8));  // only the high byte arrives
    } else if (addr >= 0xA10000 && addr < 0xA10020) {
        io_.write(addr | 1, static_cast<uint8_t>(value), now_);
    } else if (addr == 0xA11100 || addr == 0xA11200) {
        writeBusControl(addr, (value & 0x0100) != 0);
    } else if (addr >= 0xA130F0 && addr < 0xA13100) {
        cart_.writeTimeRegister(addr | 1, static_cast<uint8_t>(value));
    }
}

// ---------------------------------------------------------------------------------
// Z80 memory map

uint8_t MegaDrive::read(uint16_t addr) {
    if (addr < 0x4000) return zram_[addr & 0x1FFF];
    if (addr < 0x6000) {
        syncSound();
        return ym_.readStatus();
    }
    if (addr >= 0x8000) {
        // The banked window onto the 68000's address space.
        const uint32_t a = (static_cast<uint32_t>(z80Bank_) << 15) | (addr & 0x7FFF);
        if (a >= 0xA00000 && a < 0xA10000) return 0xFF;  // the Z80 cannot see itself
        return read8(a);
    }
    if (addr >= 0x7F00 && addr < 0x7F20) {
        const uint16_t w = readVdp(addr & 0x1E);
        return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
    return 0xFF;
}

void MegaDrive::write(uint16_t addr, uint8_t value) {
    if (addr < 0x4000) {
        zram_[addr & 0x1FFF] = value;
    } else if (addr < 0x6000) {
        syncSound();
        ym_.write(addr & 3, value);
    } else if (addr < 0x6100) {
        // Bank register: nine bits shifted in one at a time, low bit first.
        z80Bank_ = static_cast<uint16_t>(((z80Bank_ >> 1) | ((value & 1) << 8)) & 0x1FF);
    } else if (addr >= 0x7F00 && addr < 0x7F20) {
        if ((addr & 0x1F) >= 0x10) {
            syncSound();
            psg_.write(value);
        }
    } else if (addr >= 0x8000) {
        const uint32_t a = (static_cast<uint32_t>(z80Bank_) << 15) | (addr & 0x7FFF);
        if (a < 0xA00000 || a >= 0xA10000) write8(a, value);
    }
}

// ---------------------------------------------------------------------------------
// Savestates

template <class Ar>
void MegaDrive::busState(Ar& ar) {
    ar(ram_, zram_, z80Bank_, z80BusReq_, z80Reset_, now_, lineStart_, m68kClock_, z80Clock_, psgClock_,
       psgRemainder_, frames_);
}

bool MegaDrive::saveState(std::vector<uint8_t>& out) {
    StateWriter body;
    uint32_t crc = cart_.crc32();
    uint8_t region = static_cast<uint8_t>(effectiveRegion_);
    body(crc, region, frames_);
    auto block = [&](const char (&tag)[5], const std::vector<uint8_t>& data) {
        uint32_t id = fourcc(tag), size = static_cast<uint32_t>(data.size());
        body(id, size);
        body.raw(data.data(), data.size());
    };
    {
        StateWriter w;
        cpu_.state(w);
        block("M68K", w.data());
    }
    {
        StateWriter w;
        z80_.state(w);
        block("Z80 ", w.data());
    }
    {
        StateWriter w;
        vdp_.state(w);
        block("VDP ", w.data());
    }
    {
        std::vector<uint8_t> chip;
        ym_.saveState(chip);
        StateWriter w;
        uint64_t pos = ym_.clockPosition();
        w(pos, chip);
        block("YM26", w.data());
    }
    {
        StateWriter w;
        psg_.state(w);
        block("PSG ", w.data());
    }
    {
        StateWriter w;
        busState(w);
        block("BUS ", w.data());
    }
    {
        StateWriter w;
        io_.state(w);
        block("IO  ", w.data());
    }
    {
        StateWriter w;
        cart_.state(w);
        block("CART", w.data());
    }
    {
        StateWriter w;
        mixer_.state(w);
        block("MIX ", w.data());
    }
    if (!loadedThumbnail_.empty()) {
        // Nothing has run since a state was loaded: the picture on screen is still the
        // one from before, so keep the loaded state's thumbnail.
        block("THMB", loadedThumbnail_);
    } else {
        // Thumbnail: the 2D picture at half size.
        const VideoFrameMd& f = vdp_.frame();
        const int step = f.interlaced ? 2 : 1;
        uint16_t tw = static_cast<uint16_t>(f.width / 2), th = static_cast<uint16_t>(f.height / step / 2);
        StateWriter w;
        w(tw, th);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                uint32_t c = f.pixel(x * 2, y * 2 * step) & 0xFFFFFF;
                uint8_t rgb[3] = {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c)};
                w.raw(rgb, 3);
            }
        block("THMB", w.data());
    }

    mz_ulong packedSize = mz_compressBound(static_cast<mz_ulong>(body.data().size()));
    std::vector<uint8_t> packed(packedSize);
    if (mz_compress2(packed.data(), &packedSize, body.data().data(), static_cast<mz_ulong>(body.data().size()), 6) != MZ_OK)
        return false;
    packed.resize(packedSize);
    StateWriter file;
    uint32_t magic = kStateMagic, version = kStateVersion, rawSize = static_cast<uint32_t>(body.data().size());
    file(magic, version, rawSize);
    file.raw(packed.data(), packed.size());
    out = std::move(file.data());
    return true;
}

namespace {

// Unpacks a savestate file into its body. Returns an empty string on success.
std::string unpackState(const std::vector<uint8_t>& in, std::vector<uint8_t>& body) {
    if (in.size() < 12) return "el archivo no es un estado de plumsg";
    StateReader header(in.data(), 12);
    uint32_t magic = 0, version = 0, rawSize = 0;
    header(magic, version, rawSize);
    if (magic != kStateMagic) return "el archivo no es un estado de plumsg";
    if (version > kStateVersion) return "el estado es de una versión más nueva de plumsg";
    if (rawSize == 0 || rawSize > kMaxStateSize) return "el estado está dañado";
    body.resize(rawSize);
    mz_ulong size = rawSize;
    if (mz_uncompress(body.data(), &size, in.data() + 12, static_cast<mz_ulong>(in.size() - 12)) != MZ_OK || size != rawSize)
        return "el estado está dañado";
    return {};
}

}  // namespace

bool MegaDrive::loadState(const std::vector<uint8_t>& in, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    std::vector<uint8_t> body;
    const std::string problem = unpackState(in, body);
    if (!problem.empty()) return fail(problem);

    StateReader r(body.data(), body.size());
    uint32_t crc = 0;
    uint8_t region = 0;
    uint64_t frames = 0;
    r(crc, region, frames);
    if (!r.ok()) return fail("el estado está dañado");
    if (crc != cart_.crc32()) return fail("el estado es de otra ROM");

    // Keep the current state so a bad block cannot leave the console half loaded.
    std::vector<uint8_t> backup;
    saveState(backup);

    bool ok = true;
    bool hasMixer = false;
    std::vector<uint8_t> thumbnail;
    while (ok && !r.atEnd()) {
        uint32_t id = 0, size = 0;
        r(id, size);
        if (!r.ok() || size > r.remaining()) {
            ok = false;
            break;
        }
        std::vector<uint8_t> data(size);
        if (!r.raw(data.data(), size)) {
            ok = false;
            break;
        }
        StateReader b(data.data(), data.size());
        if (id == fourcc("M68K")) cpu_.state(b);
        else if (id == fourcc("Z80 ")) z80_.state(b);
        else if (id == fourcc("VDP ")) vdp_.state(b);
        else if (id == fourcc("PSG ")) psg_.state(b);
        else if (id == fourcc("BUS ")) busState(b);
        else if (id == fourcc("IO  ")) io_.state(b);
        else if (id == fourcc("CART")) cart_.state(b);
        else if (id == fourcc("THMB")) thumbnail = data;
        else if (id == fourcc("MIX ")) {
            mixer_.reset();
            mixer_.state(b);
            hasMixer = true;
        }
        else if (id == fourcc("YM26")) {
            uint64_t pos = 0;
            std::vector<uint8_t> chip;
            b(pos, chip);
            if (b.ok() && ym_.loadState(chip)) ym_.setClockPosition(pos);
            else ok = false;
        }
        // Unknown blocks (from newer versions) are skipped.
        if (!b.ok()) ok = false;
    }
    if (!ok) {
        std::string ignored;
        std::vector<uint8_t> restore;
        if (unpackState(backup, restore).empty()) {
            // Reload the backup block by block (same code path, known good).
            loadState(backup, &ignored);
        }
        return fail("el estado está dañado o es de una versión incompatible");
    }
    configureFor(static_cast<Region>(region & 3));
    if (!hasMixer) mixer_.reset();
    loadedThumbnail_ = std::move(thumbnail);
    mixer_.clearSamples();
    psgBuffer_.clear();
    ym_.samples().clear();
    psg_.clearSamples();
    frames_ = frames;
    updateIrq();
    return true;
}

bool MegaDrive::stateThumbnail(const std::vector<uint8_t>& state, std::vector<uint32_t>& pixels, int* width,
                               int* height) {
    std::vector<uint8_t> body;
    if (!unpackState(state, body).empty()) return false;
    StateReader r(body.data(), body.size());
    uint32_t crc = 0;
    uint8_t region = 0;
    uint64_t frames = 0;
    r(crc, region, frames);
    while (r.ok() && !r.atEnd()) {
        uint32_t id = 0, size = 0;
        r(id, size);
        if (!r.ok() || size > r.remaining()) return false;
        std::vector<uint8_t> data(size);
        if (!r.raw(data.data(), size)) return false;
        if (id != fourcc("THMB")) continue;
        StateReader b(data.data(), data.size());
        uint16_t w = 0, h = 0;
        b(w, h);
        if (static_cast<size_t>(w) * h * 3 > b.remaining()) return false;
        pixels.assign(static_cast<size_t>(w) * h, 0);
        for (auto& p : pixels) {
            uint8_t rgb[3];
            b.raw(rgb, 3);
            p = 0xFF000000u | static_cast<uint32_t>(rgb[0]) << 16 | static_cast<uint32_t>(rgb[1]) << 8 | rgb[2];
        }
        *width = w;
        *height = h;
        return b.ok();
    }
    return false;
}

}  // namespace plumsg
