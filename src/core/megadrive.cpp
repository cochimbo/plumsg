#include "megadrive.h"

#include "i18n.h"

#include <algorithm>
#include <cstring>

#include "miniz.h"
#include "state_io.h"
#include "system.h"

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
    vdp_.setDmaReader([this](uint32_t addr) {
        uint16_t w = 0;
        if (mcd_ && mcd_->dmaRead(addr & 0xFFFFFE, w)) return w;
        return read16(addr & 0xFFFFFE);
    });
    applyRegion();
    powerCycle();
}

bool MegaDrive::loadRom(const std::string& path, std::string* error) {
    if (detectSystem(path) == ConsoleSystem::MegaCD) return loadDisc(path, error);
    Cartridge loaded;
    if (!loaded.loadFile(path, error)) return false;
    cart_ = std::move(loaded);
    mcd_.reset();
    disc_.reset();
    romOriginal_.clear();
    ramCheats_.clear();
    setUpMars();
    applyRegion();
    powerCycle();
    return true;
}

void MegaDrive::loadRomData(std::vector<uint8_t> rom) {
    cart_.loadData(std::move(rom));
    mcd_.reset();
    disc_.reset();
    romOriginal_.clear();
    ramCheats_.clear();
    setUpMars();
    applyRegion();
    powerCycle();
}

void MegaDrive::setCdBios(Region region, std::vector<uint8_t> bios) {
    const int slot = region == Region::JapanNtsc ? 0 : region == Region::EuropePal ? 2 : 1;
    cdBios_[static_cast<size_t>(slot)] = std::move(bios);
}

bool MegaDrive::loadDisc(const std::string& path, std::string* error) {
    auto disc = std::make_unique<CdImage>();
    if (!disc->open(path, error)) return false;
    return loadDiscImage(std::move(disc), error);
}

bool MegaDrive::loadDiscImage(std::unique_ptr<CdImage> disc, std::string* error) {
    std::vector<uint8_t> header(2048);
    disc->readData(0, header.data());
    if (std::memcmp(header.data(), "SEGADISCSYSTEM", 14) != 0) {
        if (error) *error = tr("no es un disco de Mega CD");
        return false;
    }
    // The disc's first sector has a header like a cartridge's: it stands in for the
    // cartridge for the region, the title and the savestates.
    Cartridge loaded;
    loaded.loadData(header);
    // The region is the one of the security code the BIOS checks, not the header's
    // (Snatcher's says Japan on a USA disc), and the console always follows it: a
    // BIOS of another region refuses the disc and shows its CD player instead.
    const Region wanted = header[0x20B] == 0x64 ? Region::EuropePal
                          : header[0x20B] == 0xA1 ? Region::JapanNtsc
                                                  : Region::UsaNtsc;
    const int slot = wanted == Region::JapanNtsc ? 0 : wanted == Region::EuropePal ? 2 : 1;
    if (cdBios_[static_cast<size_t>(slot)].size() != 0x20000) {
        static const char* const kMessages[3] = {
            N_("Falta la BIOS del Mega CD de Japón: elígela en Consola > BIOS del Mega CD."),
            N_("Falta la BIOS del Mega CD de EE. UU.: elígela en Consola > BIOS del Mega CD."),
            N_("Falta la BIOS del Mega CD de Europa: elígela en Consola > BIOS del Mega CD.")};
        if (error) *error = tr(kMessages[slot]);
        return false;
    }
    cart_ = std::move(loaded);
    disc_ = std::move(disc);
    discRegion_ = wanted;
    mcd_ = std::make_unique<MegaCd>();
    mcd_->insert(disc_.get());
    romOriginal_.clear();
    ramCheats_.clear();
    setUpMars();
    applyRegion();
    powerCycle();
    return true;
}

std::vector<int16_t>* MegaDrive::addOnSamples() {
    if (mars_) return &mars_->samples();
    if (mcd_) return &mcd_->samples();
    return nullptr;
}

void MegaDrive::setUpMars() {
    const bool wanted = detectSystemFromData(cart_.rom()) == ConsoleSystem::Mega32X;
    if (wanted && !mars_) mars_ = std::make_unique<Mars>(cart_);
    if (!wanted) mars_.reset();
    vdp_.frame().mars.fill(0);
}

void MegaDrive::setRegion(Region r) {
    region_ = r;
    applyRegion();
}

void MegaDrive::applyRegion() {
    if (mcd_) configureFor(discRegion_);
    else configureFor(region_ == Region::Auto ? (cart_.loaded() ? cart_.preferredRegion() : Region::UsaNtsc) : region_);
}

void MegaDrive::configureFor(Region r) {
    effectiveRegion_ = r;
    const bool isPal = effectiveRegion_ == Region::EuropePal;
    vdp_.setPal(isPal);
    psg_.setClock(isPal ? Psg::kClockPal : Psg::kClockNtsc);
    const double master = isPal ? kMasterPal : kMasterNtsc;
    ym_.setMasterClock(master);
    mixer_.setFmRate(ym_.sampleRate());
    if (mars_) mars_->setSampleRate(mixer_.outputRate(), master);
    if (mcd_) {
        mcd_->setMasterClock(master);
        mcd_->setSampleRate(mixer_.outputRate());
        const int slot = r == Region::JapanNtsc ? 0 : r == Region::EuropePal ? 2 : 1;
        if (cdBios_[static_cast<size_t>(slot)].size() == 0x20000) mcd_->setBios(cdBios_[static_cast<size_t>(slot)]);
    }
    // Version register: bit 7 overseas, bit 6 PAL, bit 5 no expansion unit; version 0
    // (no TMSS).
    uint8_t version = mcd_ ? 0x00 : 0x20;
    if (effectiveRegion_ != Region::JapanNtsc) version |= 0x80;
    if (isPal) version |= 0x40;
    io_.setVersion(version);
}

void MegaDrive::setSampleRate(double hz) {
    psg_.setSampleRate(hz);
    mixer_.setOutputRate(hz);
    if (mars_) mars_->setSampleRate(hz, pal() ? kMasterPal : kMasterNtsc);
    if (mcd_) mcd_->setSampleRate(hz);
}

void MegaDrive::powerCycle() {
    ram_.fill(0);
    zram_.fill(0);
    vdp_.reset();
    if (mars_) mars_->powerOn();
    if (mcd_) mcd_->powerOn();
    reset();
}

void MegaDrive::reset() {
    cart_.reset();
    loadedThumbnail_.clear();
    io_.reset();
    ym_.reset();
    ym_.samples().clear();
    psg_.reset();
    if (roland_) roland_->allNotesOff();
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
    if (mars_) {
        mars_->reset();
        mars_->rebaseClock();
        mars_->samples().clear();
    }
    if (mcd_) {
        mcd_->reset();
        mcd_->rebaseClock();
        mcd_->samples().clear();
    }
    cpu_.setIrqLevel(0);
    cpu_.reset();
}

// ---------------------------------------------------------------------------------
// Scheduler

void MegaDrive::runUntil(uint64_t clock) {
    while (m68kClock_ < clock || z80Clock_ < clock) {
        // Run whichever CPU is further behind, one instruction at a time.
        if (m68kClock_ < clock && (m68kClock_ <= z80Clock_ || z80Clock_ >= clock)) {
            // The SH-2s catch up first, so the 68000 never sees them behind it.
            if (mars_) mars_->runUntil(m68kClock_);
            if (mcd_) mcd_->runUntil(m68kClock_);
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
    if (mars_) mars_->runUntil(clock);
    if (mcd_) mcd_->runUntil(clock);
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

void MegaDrive::setCheats(const std::vector<CheatPatch>& cheats) {
    // Undo in reverse, so a byte patched twice gets its true original back.
    for (auto it = romOriginal_.rbegin(); it != romOriginal_.rend(); ++it) cart_.patchRom(it->first, it->second, nullptr);
    romOriginal_.clear();
    ramCheats_.clear();
    for (const CheatPatch& c : cheats) {
        if (c.address >= 0xE00000) {
            ramCheats_.push_back(c);
            continue;
        }
        if (c.address >= 0x400000) continue;
        auto patch = [&](uint32_t offset, uint8_t value) {
            uint8_t old = 0;
            if (cart_.patchRom(offset, value, &old)) romOriginal_.emplace_back(offset, old);
        };
        if (c.byte) {
            patch(c.address, static_cast<uint8_t>(c.value));
        } else {
            patch(c.address, static_cast<uint8_t>(c.value >> 8));
            patch(c.address + 1, static_cast<uint8_t>(c.value));
        }
    }
    applyRamCheats();
}

void MegaDrive::applyRamCheats() {
    for (const CheatPatch& c : ramCheats_) {
        const uint32_t a = c.address & 0xFFFF;
        if (c.byte) {
            ram_[a] = static_cast<uint8_t>(c.value);
        } else {
            ram_[a] = static_cast<uint8_t>(c.value >> 8);
            ram_[(a + 1) & 0xFFFF] = static_cast<uint8_t>(c.value);
        }
    }
}

void MegaDrive::setRoland(RolandSound* roland, int chipPercent) {
    if (roland != roland_) {
        // Switching: the old one stops, the new one starts from silence.
        if (roland_) roland_->allNotesOff();
        if (roland) roland->allNotesOff();
    }
    roland_ = roland;
    rolandPercent_ = std::clamp(chipPercent, 0, 100);
    rolandChip_ = ~0u;
    rolandChipChannels();
}

// The chips play what the Roland sound does not: the instruments left original, and a
// little of the rest if the mix says so.
void MegaDrive::rolandChipChannels() {
    const uint32_t chip = roland_ ? roland_->chipChannels() : 0x3FF;
    if (chip == rolandChip_) return;
    rolandChip_ = chip;
    ym_.setMuted(0x3F & ~chip, rolandPercent_);
    psg_.setMuted(0xF & ~(chip >> 6), rolandPercent_);
}

void MegaDrive::setVgmWriter(VgmWriter* writer) {
    vgm_ = writer;
    if (vgm_) vgm_->begin(ym_, psg_, now_, masterClock());
}

void MegaDrive::runFrame() {
    applyRamCheats();
    if (roland_) roland_->beginFrame(now_);
    const int lines = vdp_.linesPerFrame();
    for (int line = 0; line < lines; ++line) {
        lineStart_ = now_;
        vdp_.startLine(line);
        if (mars_) mars_->startLine(line, vdp_.activeLines());
        updateIrq();
        z80_.setIrqLine(vdp_.z80Irq());
        if (line == vdp_.activeLines()) {
            runUntil(lineStart_ + VdpMd::kVintDelay);
            vdp_.raiseVint();
            updateIrq();
            z80_.setIrqLine(vdp_.z80Irq());
        }
        runUntil(lineStart_ + VdpMd::kActiveClocks);
        if (line < vdp_.activeLines() && drawing_) {
            vdp_.renderLine(line);
            if (mars_) mars_->renderLine(line, vdp_.frame());
        }
        if (mars_) mars_->startHblank(line, vdp_.activeLines());
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
    const size_t before = mixer_.samples().size() / 2;
    mixer_.mix(ym_.samples(), psgBuffer_, addOnSamples());
    if (roland_) roland_->endFrame(now_, mixer_.output(), before, mixer_.outputRate());
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
            if (vgm_) vgm_->psg(static_cast<uint8_t>(value), now_);
            if (roland_) {
                roland_->psgWrite(psg_, now_);
                rolandChipChannels();
            }
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
    fromM68k_ = true;
    write(static_cast<uint16_t>(addr & 0x7FFF), value);
    fromM68k_ = false;
}

void MegaDrive::setMemoryWatch(MemoryWatch watch) {
    watch_ = std::move(watch);
    watching_ = watch_ || lockCount_ > 0;
}

void MegaDrive::lockSoundRequests(uint32_t addr, int count) {
    lockAddr_ = addr;
    lockCount_ = count;
    watching_ = watch_ || lockCount_ > 0;
}

void MegaDrive::poke(uint32_t addr, uint8_t value) {
    if (addr >= 0xFF0000)
        ram_[addr & 0xFFFF] = value;
    else if (addr >= 0xA00000 && addr < 0xA04000)
        zram_[addr & 0x1FFF] = value;
}

bool MegaDrive::watchWrite(uint32_t addr, uint8_t value, bool m68k) {
    if (m68k && value != 0 && value != 0x80 && addr - lockAddr_ < static_cast<uint32_t>(lockCount_)) return false;
    if (watch_) watch_(addr, value, m68k);
    return true;
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
            if (vgm_) vgm_->fmReset(now_);
            if (roland_) roland_->keysOff(now_);
        }
        if (!reset && z80Reset_) z80Clock_ = std::max(z80Clock_, now_);
        z80Reset_ = reset;
    }
}

uint8_t MegaDrive::read8(uint32_t addr) {
    addr &= 0xFFFFFF;
    if (mars_) {
        uint16_t w = 0;
        if (mars_->read68k(addr, w)) return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
    if (mcd_) {
        uint16_t w = 0;
        if (mcd_->read68k(addr, w)) return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
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
    if (mars_) {
        uint16_t w = 0;
        if (mars_->read68k(addr, w)) return w;
    }
    if (mcd_) {
        uint16_t w = 0;
        if (mcd_->read68k(addr, w)) return w;
    }
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
    if (mars_ && mars_->write68k(addr, static_cast<uint16_t>(value << 8 | value), !(addr & 1), (addr & 1) != 0)) return;
    if (mcd_ && mcd_->write68k(addr & ~1u, static_cast<uint16_t>(value << 8 | value), !(addr & 1), (addr & 1) != 0)) return;
    if (addr >= 0xE00000) {
        if (watching_ && !watchWrite(0xFF0000 | addr, value, true)) return;
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
    if (mars_ && mars_->write68k(addr, value, true, true)) return;
    if (mcd_ && mcd_->write68k(addr, value, true, true)) return;
    if (addr >= 0xE00000) {
        if (watching_) {
            if (watchWrite(0xFF0000 | addr, static_cast<uint8_t>(value >> 8), true))
                ram_[addr & 0xFFFF] = static_cast<uint8_t>(value >> 8);
            if (watchWrite(0xFF0000 | (addr + 1), static_cast<uint8_t>(value), true))
                ram_[(addr & 0xFFFF) + 1] = static_cast<uint8_t>(value);
            return;
        }
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
        if (watching_ && !watchWrite(0xA00000 | (addr & 0x1FFF), value, fromM68k_)) return;
        zram_[addr & 0x1FFF] = value;
    } else if (addr < 0x6000) {
        syncSound();
        if (watch_ && fromM68k_) watch_(0xA04000 | (addr & 3), value, true);  // who drives the YM2612
        ym_.write(addr & 3, value);
        if (vgm_) vgm_->fm(addr & 3, value, now_);
        if (roland_) {
            roland_->fmWrite(addr & 3, value, ym_, now_);
            rolandChipChannels();
        }
    } else if (addr < 0x6100) {
        // Bank register: nine bits shifted in one at a time, low bit first.
        z80Bank_ = static_cast<uint16_t>(((z80Bank_ >> 1) | ((value & 1) << 8)) & 0x1FF);
    } else if (addr >= 0x7F00 && addr < 0x7F20) {
        if ((addr & 0x1F) >= 0x10) {
            syncSound();
            psg_.write(value);
            if (vgm_) vgm_->psg(value, now_);
            if (roland_) {
                roland_->psgWrite(psg_, now_);
                rolandChipChannels();
            }
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

bool MegaDrive::saveState(std::vector<uint8_t>& out, bool quick) {
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
    if (mars_ || mcd_) {
        StateWriter w;
        mixer_.addOnState(w);
        block("MIXA", w.data());
    }
    if (mars_) {
        StateWriter w;
        mars_->state(w);
        block("32X ", w.data());
    }
    if (mcd_) {
        StateWriter w;
        mcd_->state(w);
        block("MCD ", w.data());
    }
    if (quick) {
        // No thumbnail: nobody looks at a rewind snapshot.
    } else if (!loadedThumbnail_.empty()) {
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
    if (mz_compress2(packed.data(), &packedSize, body.data().data(), static_cast<mz_ulong>(body.data().size()), quick ? 1 : 6) != MZ_OK)
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
    if (in.size() < 12) return tr("el archivo no es un estado de plumsg");
    StateReader header(in.data(), 12);
    uint32_t magic = 0, version = 0, rawSize = 0;
    header(magic, version, rawSize);
    if (magic != kStateMagic) return tr("el archivo no es un estado de plumsg");
    if (version > kStateVersion) return tr("el estado es de una versión más nueva de plumsg");
    if (rawSize == 0 || rawSize > kMaxStateSize) return tr("el estado está dañado");
    body.resize(rawSize);
    mz_ulong size = rawSize;
    if (mz_uncompress(body.data(), &size, in.data() + 12, static_cast<mz_ulong>(in.size() - 12)) != MZ_OK || size != rawSize)
        return tr("el estado está dañado");
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
    if (!r.ok()) return fail(tr("el estado está dañado"));
    if (crc != cart_.crc32()) return fail(tr("el estado es de otra ROM"));

    // Keep the current state so a bad block cannot leave the console half loaded.
    std::vector<uint8_t> backup;
    saveState(backup, true);

    bool ok = true;
    bool hasMixer = false;
    std::vector<uint8_t> thumbnail, addOnState;
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
        if (id == fourcc("M68K")) {
            cpu_.state(b);
            cpu_.forgetRoutine();
        } else if (id == fourcc("Z80 ")) z80_.state(b);
        else if (id == fourcc("VDP ")) vdp_.state(b);
        else if (id == fourcc("PSG ")) psg_.state(b);
        else if (id == fourcc("BUS ")) busState(b);
        else if (id == fourcc("IO  ")) io_.state(b);
        else if (id == fourcc("CART")) cart_.state(b);
        else if (id == fourcc("THMB")) thumbnail = data;
        else if (id == fourcc("32X ")) {
            if (mars_) mars_->state(b);
        }
        else if (id == fourcc("MCD ")) {
            if (mcd_) mcd_->state(b);
        }
        else if (id == fourcc("MIXA")) addOnState = data;
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
            // Reload the backup block by block (same code path, known good). The quick
            // backup has no thumbnail, so keep the one that was there.
            std::vector<uint8_t> keptThumbnail = loadedThumbnail_;
            loadState(backup, &ignored);
            loadedThumbnail_ = std::move(keptThumbnail);
        }
        return fail(tr("el estado está dañado o es de una versión incompatible"));
    }
    configureFor(static_cast<Region>(region & 3));
    if (!hasMixer) mixer_.reset();
    if (!addOnState.empty()) {
        // After the main mixer block, which empties the queue.
        StateReader b(addOnState.data(), addOnState.size());
        mixer_.addOnState(b);
    }
    loadedThumbnail_ = std::move(thumbnail);
    mixer_.clearSamples();
    psgBuffer_.clear();
    ym_.samples().clear();
    psg_.clearSamples();
    if (mars_) mars_->samples().clear();
    if (mcd_) mcd_->samples().clear();
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
