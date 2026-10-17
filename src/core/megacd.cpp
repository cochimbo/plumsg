#include "megacd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace plumsg {

// What follows is modelled on ares's Mega CD (ISC licence, Near and contributors).

namespace {

// CDD status and command codes.
enum CddStatus : uint8_t {
    kStopped = 0x0,
    kPlaying = 0x1,
    kSeeking = 0x2,
    kPaused = 0x4,
    kChecksumError = 0x6,
    kCommandError = 0x7,
    kReadingToc = 0x9,
    kTracking = 0xA,
    kNoDisc = 0xB,
    kLeadOut = 0xC,
    kLeadIn = 0xD,
};

// The drive reads its table of contents from these sectors before track 1's pregap.
constexpr int32_t kLeadInStart = -CdImage::kPregap - 150;
constexpr int32_t kLeadInEnd = -CdImage::kPregap - 1;
constexpr int32_t kLeadOutSectors = 6750;

// CDC control bits (CTRL0 and CTRL1).
enum : uint32_t {
    kHead = 1u << 0,  // HEAD registers show the subheader instead of the header
    kForm = 1u << 2,
    kMode = 1u << 3,
    kWriteRequest = 1u << 10,
    kAutoCorrection = 1u << 12,
};

int clamp16(int v) { return std::clamp(v, -32768, 32767); }

void msf(int32_t frames, int& m, int& s, int& f) {
    if (frames < 0) frames += 100 * 60 * 75;
    m = frames / (60 * 75) % 100;
    s = frames / 75 % 60;
    f = frames % 75;
}

}  // namespace

MegaCd::MegaCd() {
    powerOn();
    formatBackupRam();  // until the frontend loads the saved one
}

void MegaCd::insert(CdImage* disc) {
    disc_ = disc;
    cachedLba_ = kNoSector;
    cddInsert();
}

void MegaCd::powerOn() {
    prg_.fill(0);
    wram_.fill(0);
    subClock_ = 0;
    subcodeIrq_ = {};
    io_.vectorLevel4 = 0xFFFFFFFF;
    reset();
    io_.vectorLevel4 = 0xFFFFFFFF;
}

void MegaCd::reset() {
    resetCpu();
    const uint32_t vector = io_.vectorLevel4;
    io_ = {};
    io_.vectorLevel4 = vector;
    counter_ = {};
    led_ = 0;
    external_ = {};
    comm_ = {};
    cdcPower();
    timer_ = {};
    gpu_ = {};
    pcm_.ram.fill(0);
    pcm_.enable = false;
    pcm_.bank = pcm_.channel = 0;
    pcm_.channels.fill(PcmChannel{});
    resetPeripheral();
    updateIrq();
}

void MegaCd::resetCpu() {
    sub_.setIrqLevel(0);
    resetPending_ = true;
}

void MegaCd::resetPeripheral() { cddPower(); }

void MegaCd::formatBackupRam() {
    // An empty directory, as the BIOS's own formatter leaves it.
    static const uint8_t kFormat[0x40] = {
        0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x00, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x7D, 0x00, 0x7D, 0x00, 0x7D, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x53, 0x45, 0x47, 0x41, 0x5F, 0x43, 0x44, 0x5F, 0x52, 0x4F, 0x4D, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x52, 0x41, 0x4D, 0x5F, 0x43, 0x41, 0x52, 0x54, 0x52, 0x49, 0x44, 0x47, 0x45, 0x5F, 0x5F, 0x5F};
    bram_.fill(0);
    std::memcpy(&bram_[bram_.size() - sizeof kFormat], kFormat, sizeof kFormat);
}

// ---------------------------------------------------------------------------------
// Scheduling

void MegaCd::runUntil(uint64_t masterClock) {
    const uint64_t target = static_cast<uint64_t>(static_cast<double>(masterClock) * kSubClock / master_);
    while (subClock_ < target) {
        int cycles = 16;
        if (!io_.halt) {
            if (resetPending_) {
                resetPending_ = false;
                sub_.reset();
            }
            cycles = sub_.step();
        }
        step(cycles);
        subClock_ += static_cast<uint64_t>(cycles);
    }
}

void MegaCd::step(int clocks) {
    gpuStep(clocks);
    counter_.divider += static_cast<uint32_t>(clocks);
    while (counter_.divider >= 384) {
        counter_.divider -= 384;
        cdc_.stopwatch = (cdc_.stopwatch + 1) & 0xFFF;
        cddClock();
        if (timer_.frequency && !timer_.counter--) {
            timer_.counter = timer_.frequency;
            timer_.irq.raise();
        }
        pcmClock();
    }
    counter_.dma += static_cast<uint32_t>(clocks);
    while (counter_.dma >= 6) {
        counter_.dma -= 6;
        cdcDma();
    }
    counter_.cdda += clocks;
    while (counter_.cdda >= kSubClock / 44100.0) {
        counter_.cdda -= kSubClock / 44100.0;
        cddSample();
    }
    updateIrq();
}

void MegaCd::updateIrq() {
    int level = 0;
    if (gpu_.irq.pending) level = 1;
    if (external_.pending) level = 2;
    if (timer_.irq.pending) level = 3;
    if (cdd_.irq.pending) level = 4;
    if (cdc_.irq.pending) level = 5;
    if (subcodeIrq_.pending) level = 6;
    sub_.setIrqLevel(level);
}

void MegaCd::irqAcknowledge(int level) {
    switch (level) {
        case 1: gpu_.irq.lower(); break;
        case 2: external_.lower(); break;
        case 3: timer_.irq.lower(); break;
        case 4: cdd_.irq.lower(); break;
        case 5: cdc_.irq.lower(); break;
        case 6: subcodeIrq_.lower(); break;
        default: break;
    }
    updateIrq();
}

// ---------------------------------------------------------------------------------
// Sub CPU bus

uint8_t MegaCd::read8(uint32_t addr) {
    const uint16_t w = subRead(addr & ~1u, !(addr & 1), (addr & 1) != 0);
    return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
}

uint16_t MegaCd::read16(uint32_t addr) { return subRead(addr, true, true); }

void MegaCd::write8(uint32_t addr, uint8_t value) {
    subWrite(addr & ~1u, static_cast<uint16_t>(value << 8 | value), !(addr & 1), (addr & 1) != 0);
}

void MegaCd::write16(uint32_t addr, uint16_t value) { subWrite(addr, value, true, true); }

uint16_t MegaCd::subRead(uint32_t addr, bool upper, bool lower) {
    addr &= 0xFFFFFE;
    if (addr < 0x080000) return static_cast<uint16_t>(prg_[addr] << 8 | prg_[addr + 1]);
    if (addr < 0x0C0000) {
        if (!io_.wramMode) {
            if (!io_.wramSwitch) return 0;
            return wram_[(addr & 0x3FFFF) >> 1];
        }
        // Dot-mapped window: each word is two pixels of one byte.
        const int shift = (addr & 2) ? 0 : 8;
        const uint16_t w = wram_[(((addr >> 1) & 0x1FFFF) & ~1u) | (io_.wramSelect ? 0 : 1)];
        return static_cast<uint16_t>(((w >> (shift + 4)) & 0xF) << 8 | ((w >> shift) & 0xF));
    }
    if (addr < 0x0E0000) {
        if (!io_.wramMode) return 0;
        return wram_[((addr & 0x1FFFF) & ~1u) | (io_.wramSelect ? 0 : 1)];
    }
    if (addr >= 0xFE0000 && addr < 0xFF0000) return lower ? bram_[(addr >> 1) & 0x1FFF] : 0;
    if (addr >= 0xFF0000 && addr < 0xFF8000) return lower ? pcmRead((addr >> 1) & 0x1FFF) : 0;
    if (addr >= 0xFF8000) return readIo(addr, upper, lower);
    return 0;
}

void MegaCd::subWrite(uint32_t addr, uint16_t value, bool upper, bool lower) {
    addr &= 0xFFFFFE;
    if (addr < 0x080000) {
        if (addr < static_cast<uint32_t>(io_.prgProtect) << 9) return;
        if (upper) prg_[addr] = static_cast<uint8_t>(value >> 8);
        if (lower) prg_[addr + 1] = static_cast<uint8_t>(value);
        return;
    }
    if (addr < 0x0C0000) {
        if (!io_.wramMode) {
            if (!io_.wramSwitch) return;  // the console has it
            uint16_t& w = wram_[(addr & 0x3FFFF) >> 1];
            if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (value & 0xFF00));
            if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (value & 0x00FF));
            return;
        }
        const int shift = (addr & 2) ? 0 : 8;
        uint16_t& w = wram_[(((addr >> 1) & 0x1FFFF) & ~1u) | (io_.wramSelect ? 0 : 1)];
        auto put = [&](int at, int pixel) {
            const int old = (w >> at) & 0xF;
            if (io_.wramPriority == 1 && old) return;    // under write
            if (io_.wramPriority == 2 && !pixel) return;  // over write
            if (io_.wramPriority == 3) return;
            w = static_cast<uint16_t>((w & ~(0xF << at)) | pixel << at);
        };
        if (upper) put(shift + 4, (value >> 8) & 0xF);
        if (lower) put(shift, value & 0xF);
        return;
    }
    if (addr < 0x0E0000) {
        if (!io_.wramMode) return;
        uint16_t& w = wram_[((addr & 0x1FFFF) & ~1u) | (io_.wramSelect ? 0 : 1)];
        if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (value & 0xFF00));
        if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (value & 0x00FF));
        return;
    }
    if (addr >= 0xFE0000 && addr < 0xFF0000) {
        if (!lower) return;
        uint8_t& b = bram_[(addr >> 1) & 0x1FFF];
        if (b != static_cast<uint8_t>(value)) bramChanged_ = true;
        b = static_cast<uint8_t>(value);
        return;
    }
    if (addr >= 0xFF0000 && addr < 0xFF8000) {
        if (lower) pcmWrite((addr >> 1) & 0x1FFF, static_cast<uint8_t>(value));
        return;
    }
    if (addr >= 0xFF8000) writeIo(addr, value, upper, lower);
}

uint16_t MegaCd::readIo(uint32_t addr, bool upper, bool lower) {
    addr = 0xFF8000 | (addr & 0x1FF);
    uint16_t d = 0;
    switch (addr) {
        case 0xFF8000: return static_cast<uint16_t>(1 | led_ << 8);
        case 0xFF8002: {
            const bool b0 = !io_.wramMode ? !io_.wramSwitch : io_.wramSelect;
            const bool b1 = !io_.wramMode ? io_.wramSwitch : io_.wramSwitchRequest;
            return static_cast<uint16_t>(b0 | b1 << 1 | io_.wramMode << 2 | io_.wramPriority << 3 | io_.prgProtect << 8);
        }
        case 0xFF8004:
            return static_cast<uint16_t>(cdc_.address | cdc_.transfer.destination << 8 | cdc_.transfer.ready << 14 |
                                         cdc_.transfer.completed << 15);
        case 0xFF8006: return lower ? cdcRead() : 0;
        case 0xFF8008: return cdcTransferRead();
        case 0xFF800A: return static_cast<uint16_t>(cdc_.transfer.address >> 3);
        case 0xFF800C: return cdc_.stopwatch;
        case 0xFF800E: return static_cast<uint16_t>(comm_.cfm << 8 | comm_.cfs);
        case 0xFF8030: return timer_.frequency;
        case 0xFF8032:
            return static_cast<uint16_t>(gpu_.irq.enable << 1 | external_.enable << 2 | timer_.irq.enable << 3 |
                                         cdd_.irq.enable << 4 | cdc_.irq.enable << 5 | subcodeIrq_.enable << 6);
        case 0xFF8034: return 0;
        case 0xFF8036: return static_cast<uint16_t>(cdd_.hostClockEnable << 2 | trackData(cdd_.track) << 8);
        case 0xFF804C: return static_cast<uint16_t>(gpu_.fontBackground | gpu_.fontForeground << 4);
        case 0xFF804E: return gpu_.fontData;
        case 0xFF8058: return static_cast<uint16_t>(gpu_.repeat | gpu_.tileSize << 1 | gpu_.mapSize << 2 | gpu_.active << 15);
        case 0xFF805A: return static_cast<uint16_t>(((gpu_.mapBase >> 9) & 0x1FF) << 7);
        case 0xFF805C: return gpu_.imageVcells;
        case 0xFF805E: return static_cast<uint16_t>(((gpu_.imageBase >> 5) & 0x1FFF) << 3);
        case 0xFF8060: return gpu_.imageOffset;
        case 0xFF8062: return gpu_.imageHdots;
        case 0xFF8064: return gpu_.imageVdots;
        case 0xFF8066: return static_cast<uint16_t>(((gpu_.vectorBase >> 3) & 0x7FFF) << 1);
        case 0xFF8068: return static_cast<uint16_t>((cdd_.subcodePosition & 0x7F) << 1);
        default: break;
    }
    if (addr >= 0xFF8010 && addr < 0xFF8020) return comm_.command[(addr - 0xFF8010) >> 1];
    if (addr >= 0xFF8020 && addr < 0xFF8030) return comm_.status[(addr - 0xFF8020) >> 1];
    if (addr >= 0xFF8038 && addr < 0xFF8042) {
        const uint32_t i = addr - 0xFF8038;
        return static_cast<uint16_t>(cdd_.statusRegs[i] << 8 | cdd_.statusRegs[i | 1]);
    }
    if (addr >= 0xFF8042 && addr < 0xFF804C) {
        const uint32_t i = addr - 0xFF8042;
        return static_cast<uint16_t>(cdd_.commandRegs[i] << 8 | cdd_.commandRegs[i | 1]);
    }
    if (addr >= 0xFF8050 && addr < 0xFF8058) {
        // The font registers' two bits per pixel turned into colours.
        const int index = 12 - static_cast<int>((addr - 0xFF8050) << 1);
        for (int i = 0; i < 4; ++i) {
            const bool bit = (gpu_.fontData >> (index + i)) & 1;
            d |= static_cast<uint16_t>((bit ? gpu_.fontForeground : gpu_.fontBackground) << (i * 4));
        }
        return d;
    }
    if (addr >= 0xFF8100) return cdd_.subcode[((addr - 0xFF8100) >> 1) & 63];
    (void)upper;
    return 0;
}

void MegaCd::writeIo(uint32_t addr, uint16_t value, bool upper, bool lower) {
    addr = 0xFF8000 | (addr & 0x1FF);
    const uint8_t low = static_cast<uint8_t>(value);
    switch (addr) {
        case 0xFF8000:
            if (lower && !(value & 1)) resetPeripheral();
            if (upper) led_ = static_cast<uint8_t>((value >> 8) & 3);
            return;
        case 0xFF8002:
            if (io_.wramSelect != ((low & 1) != 0)) io_.wramSwitchRequest = false;
            io_.wramSelect = (low & 1) != 0;
            io_.wramMode = (low & 4) != 0;
            io_.wramPriority = static_cast<uint8_t>((low >> 3) & 3);
            if (io_.wramSelect) io_.wramSwitch = false;
            return;
        case 0xFF8004:
            if (lower) cdc_.address = low & 0x1F;
            if (upper) cdc_.transfer.destination = static_cast<uint8_t>((value >> 8) & 7);
            return;
        case 0xFF8006:
            if (lower) cdcWrite(low);
            updateIrq();
            return;
        case 0xFF800A: cdc_.transfer.address = static_cast<uint32_t>(value) << 3; return;
        case 0xFF800C: cdc_.stopwatch = 0; return;
        case 0xFF800E: comm_.cfs = low; return;
        case 0xFF8030: timer_.counter = timer_.frequency = low; return;
        case 0xFF8032:
            if (lower) {
                gpu_.irq.enable = (low >> 1) & 1;
                external_.enable = (low >> 2) & 1;
                timer_.irq.enable = (low >> 3) & 1;
                cdd_.irq.enable = (low >> 4) & 1;
                cdc_.irq.enable = (low >> 5) & 1;
                subcodeIrq_.enable = (low >> 6) & 1;
            }
            return;
        case 0xFF8034:
            cdd_.dacRate = (value >> 1) & 1;
            cdd_.dacDeemphasis = static_cast<uint8_t>((value >> 2) & 3);
            cdd_.attenuator = static_cast<uint16_t>((cdd_.attenuator & 0x3F) | (value & 0xFFC0));
            return;
        case 0xFF8036:
            if (lower) {
                if (!cdd_.hostClockEnable && (low & 4)) cdd_.irq.raise();
                cdd_.hostClockEnable = (low & 4) != 0;
                cdd_.counter = 0;
                updateIrq();
            }
            return;
        case 0xFF804C:
            gpu_.fontBackground = low & 0xF;
            gpu_.fontForeground = low >> 4;
            return;
        case 0xFF804E:
            if (lower) gpu_.fontData = static_cast<uint16_t>((gpu_.fontData & 0xFF00) | low);
            if (upper) gpu_.fontData = static_cast<uint16_t>((gpu_.fontData & 0x00FF) | (value & 0xFF00));
            return;
        case 0xFF8058:
            if (lower) {
                gpu_.repeat = low & 1;
                gpu_.tileSize = (low >> 1) & 1;
                gpu_.mapSize = (low >> 2) & 1;
            }
            return;
        case 0xFF805A: gpu_.mapBase = static_cast<uint32_t>((value >> 5) & 0x7FF) << 7; return;
        case 0xFF805C: gpu_.imageVcells = low & 0x1F; return;
        case 0xFF805E: gpu_.imageBase = static_cast<uint32_t>((value >> 3) & 0x1FFF) << 5; return;
        case 0xFF8060: gpu_.imageOffset = low & 0x3F; return;
        case 0xFF8062: gpu_.imageHdots = value & 0x1FF; return;
        case 0xFF8064: gpu_.imageVdots = low; return;
        case 0xFF8066:
            gpu_.vectorBase = static_cast<uint32_t>((value >> 1) & 0x7FFF) << 3;
            gpuStart();
            return;
        default: break;
    }
    if (addr >= 0xFF8020 && addr < 0xFF8030) {
        uint16_t& s = comm_.status[(addr - 0xFF8020) >> 1];
        if (lower) s = static_cast<uint16_t>((s & 0xFF00) | low);
        if (upper) s = static_cast<uint16_t>((s & 0x00FF) | (value & 0xFF00));
        return;
    }
    if (addr >= 0xFF8042 && addr < 0xFF804C) {
        const uint32_t i = addr - 0xFF8042;
        if (lower) cdd_.commandRegs[i | 1] = low & 0xF;
        if (upper) cdd_.commandRegs[i] = (value >> 8) & 0xF;
        if (lower && (i | 1) == 9) cddProcess();
    }
}

// ---------------------------------------------------------------------------------
// The console's side

bool MegaCd::read68k(uint32_t addr, uint16_t& value) {
    if (addr < 0x400000) {
        value = readExternal(addr & ~1u);
        return true;
    }
    if ((addr & 0xFFFF00) == 0xA12000) {
        value = readExternalIo(addr & 0xFFFF3E);
        return true;
    }
    return false;
}

bool MegaCd::write68k(uint32_t addr, uint16_t value, bool upper, bool lower) {
    if (addr < 0x400000) {
        writeExternal(addr & ~1u, value, upper, lower);
        return true;
    }
    if ((addr & 0xFFFF00) == 0xA12000) {
        writeExternalIo(addr & 0xFFFF3E, value, upper, lower);
        return true;
    }
    return false;
}

uint32_t MegaCd::wordRamIndex(uint32_t addr) const {
    if (!io_.wramMode) return io_.wramSwitch ? UINT32_MAX : (addr & 0x3FFFF) >> 1;
    if (addr >= 0x220000) {
        // Cell-mapped windows: 8x8 pixel cells laid out for the VDP, in columns of
        // 32, 16, 8 or 4 cells.
        if (addr < 0x230000) addr = (addr & 0x0FC00) >> 8 | (addr & 0x003FC) << 6 | (addr & 0x10002);
        else if (addr < 0x238000) addr = (addr & 0x07E00) >> 7 | (addr & 0x001FC) << 6 | (addr & 0x18002);
        else if (addr < 0x23C000) addr = (addr & 0x03F00) >> 6 | (addr & 0x000FC) << 6 | (addr & 0x1C002);
        else addr = (addr & 0x01F80) >> 5 | (addr & 0x0007C) << 6 | (addr & 0x1E002);
    }
    return ((addr & 0x1FFFF) & ~1u) | (io_.wramSelect ? 1 : 0);
}

bool MegaCd::dmaRead(uint32_t addr, uint16_t& value) {
    addr &= 0x3BFFFE;
    if (addr < 0x200000 || addr >= 0x240000) return false;
    const uint32_t i = wordRamIndex(addr);
    value = io_.wramLatch;
    io_.wramLatch = i == UINT32_MAX ? 0 : wram_[i];
    return true;
}

uint16_t MegaCd::readExternal(uint32_t addr) {
    addr &= 0x3BFFFE;  // bits 18-20 are not decoded
    if (addr < 0x020000) {
        if (addr == 0x70) return static_cast<uint16_t>(io_.vectorLevel4 >> 16);
        if (addr == 0x72) return static_cast<uint16_t>(io_.vectorLevel4);
        if (bios_.size() < 0x20000) return 0;
        return static_cast<uint16_t>(bios_[addr] << 8 | bios_[addr + 1]);
    }
    if (addr < 0x040000) {
        if (!io_.request) return 0;
        const uint32_t a = static_cast<uint32_t>(io_.prgBank) << 17 | (addr & 0x1FFFF);
        return static_cast<uint16_t>(prg_[a] << 8 | prg_[a + 1]);
    }
    if (addr >= 0x200000 && addr < 0x240000) {
        const uint32_t i = wordRamIndex(addr);
        return i == UINT32_MAX ? 0 : wram_[i];
    }
    return 0;
}

void MegaCd::writeExternal(uint32_t addr, uint16_t value, bool upper, bool lower) {
    addr &= 0x3BFFFE;
    if (addr >= 0x020000 && addr < 0x040000) {
        if (!io_.request) return;
        const uint32_t a = static_cast<uint32_t>(io_.prgBank) << 17 | (addr & 0x1FFFF);
        if (upper) prg_[a] = static_cast<uint8_t>(value >> 8);
        if (lower) prg_[a + 1] = static_cast<uint8_t>(value);
        return;
    }
    if (addr >= 0x200000 && addr < 0x240000) {
        const uint32_t i = wordRamIndex(addr);
        if (i == UINT32_MAX) return;
        uint16_t& w = wram_[i];
        if (upper) w = static_cast<uint16_t>((w & 0x00FF) | (value & 0xFF00));
        if (lower) w = static_cast<uint16_t>((w & 0xFF00) | (value & 0x00FF));
    }
}

uint16_t MegaCd::readExternalIo(uint32_t addr) {
    switch (addr) {
        case 0xA12000:
            return static_cast<uint16_t>(io_.run | io_.request << 1 | external_.pending << 8 | external_.enable << 15);
        case 0xA12002: {
            const bool b0 = !io_.wramMode ? !io_.wramSwitch : io_.wramSelect;
            const bool b1 = !io_.wramMode ? io_.wramSwitch : io_.wramSwitchRequest;
            return static_cast<uint16_t>(b0 | b1 << 1 | io_.wramMode << 2 | io_.prgBank << 6 | io_.prgProtect << 8);
        }
        case 0xA12004:
            return static_cast<uint16_t>(cdc_.transfer.destination << 8 | cdc_.transfer.ready << 14 |
                                         cdc_.transfer.completed << 15);
        case 0xA12006: return static_cast<uint16_t>(io_.vectorLevel4);
        case 0xA12008: return cdc_.transfer.destination == 2 ? cdcTransferRead() : 0;
        case 0xA1200C: return cdc_.stopwatch;
        case 0xA1200E: return static_cast<uint16_t>(comm_.cfm << 8 | comm_.cfs);
        default: break;
    }
    if (addr >= 0xA12010 && addr < 0xA12020) return comm_.command[(addr - 0xA12010) >> 1];
    if (addr >= 0xA12020 && addr < 0xA12030) return comm_.status[(addr - 0xA12020) >> 1];
    return 0;
}

void MegaCd::writeExternalIo(uint32_t addr, uint16_t value, bool upper, bool lower) {
    const uint8_t low = static_cast<uint8_t>(value);
    switch (addr) {
        case 0xA12000:
            if (lower) {
                if (io_.run && !(low & 1)) resetCpu();
                io_.run = (low & 1) != 0;
                io_.request = io_.run ? (low & 2) != 0 : true;  // holding it in reset also takes its bus
                io_.halt = io_.request;
            }
            if (upper && (value & 0x0100)) {
                external_.raise();
                updateIrq();
            }
            return;
        case 0xA12002:
            if (lower) {
                if (!(low & 2)) io_.wramSwitchRequest = true;
                else io_.wramSwitch = true;
                io_.prgBank = static_cast<uint8_t>((low >> 6) & 3);
            }
            if (upper) io_.prgProtect = static_cast<uint8_t>(value >> 8);
            return;
        case 0xA12006:
            if (upper) io_.vectorLevel4 = (io_.vectorLevel4 & 0xFFFF00FF) | (value & 0xFF00);
            if (lower) io_.vectorLevel4 = (io_.vectorLevel4 & 0xFFFFFF00) | low;
            return;
        case 0xA1200E: comm_.cfm = static_cast<uint8_t>(value >> 8); return;
        default: break;
    }
    if (addr >= 0xA12010 && addr < 0xA12020) {
        uint16_t& c = comm_.command[(addr - 0xA12010) >> 1];
        if (lower) c = static_cast<uint16_t>((c & 0xFF00) | low);
        if (upper) c = static_cast<uint16_t>((c & 0x00FF) | (value & 0xFF00));
    }
}

// ---------------------------------------------------------------------------------
// CDC: Sanyo LC8951, emulated as a controller that never meets a read error.

void MegaCd::cdcPoll() {
    const bool pending = (cdc_.decoderIrq.enable && cdc_.decoderIrq.pending) ||
                         (cdc_.transferIrq.enable && cdc_.transferIrq.pending) ||
                         (cdc_.commandIrq.enable && cdc_.commandIrq.pending);
    if (pending) cdc_.irq.raise();
    else cdc_.irq.lower();
}

void MegaCd::cdcDecode(int32_t sector) {
    if (!cdc_.decoderEnable || !disc_) return;
    int m, s, f;
    msf(sector + CdImage::kPregap, m, s, f);
    cdc_.header = {static_cast<uint8_t>(m / 10 << 4 | m % 10), static_cast<uint8_t>(s / 10 << 4 | s % 10),
                   static_cast<uint8_t>(f / 10 << 4 | f % 10), 0x01};
    cdc_.decoderValid = true;
    cdc_.decoderIrq.pending = true;
    cdcPoll();
    if (cdc_.control & kWriteRequest) {
        Transfer& t = cdc_.transfer;
        t.pointer = static_cast<uint16_t>(t.pointer + 2352);
        t.target = static_cast<uint16_t>(t.target + 2352);
        cachedSector(sector);
        // The sync pattern lands after the rest of the sector, not before it.
        for (uint32_t i = 0; i < 12; i += 2)
            cdc_.ram[((t.pointer + i + 2340) >> 1) & 0x1FFF] = static_cast<uint16_t>(sector_[i] << 8 | sector_[i + 1]);
        for (uint32_t i = 0; i < 2340; i += 2)
            cdc_.ram[((t.pointer + i) >> 1) & 0x1FFF] = static_cast<uint16_t>(sector_[12 + i] << 8 | sector_[13 + i]);
    }
}

uint8_t MegaCd::cdcRead() {
    uint8_t d = 0;
    Transfer& t = cdc_.transfer;
    const bool head = (cdc_.control & kHead) != 0;
    switch (cdc_.address) {
        case 0x0:  // COMIN
            if (cdc_.commandEmpty) {
                d = 0xFF;
                break;
            }
            d = cdc_.commandFifo[cdc_.commandRead];
            cdc_.commandRead = (cdc_.commandRead + 1) & 7;
            if (cdc_.commandRead == cdc_.commandWrite) {
                cdc_.commandEmpty = true;
                cdc_.commandIrq.pending = false;
                cdcPoll();
            }
            break;
        case 0x1:  // IFSTAT
            // Active-low flags.
            d = static_cast<uint8_t>(int{!cdc_.statusActive} | int{!t.active} << 1 | int{!cdc_.statusBusy} << 2 |
                                     int{!t.busy} << 3 | 1 << 4 | int{!cdc_.decoderIrq.pending} << 5 |
                                     int{!cdc_.transferIrq.pending} << 6 | int{!cdc_.commandIrq.pending} << 7);
            break;
        case 0x2: d = static_cast<uint8_t>(t.length); break;               // DBCL
        case 0x3: d = static_cast<uint8_t>((t.length >> 8) & 0xF); break;  // DBCH
        case 0x4: case 0x5: case 0x6: case 0x7:                            // HEAD0-3
            d = head ? cdc_.subheader[cdc_.address - 4] : cdc_.header[cdc_.address - 4];
            break;
        case 0x8: d = static_cast<uint8_t>(t.pointer); break;       // PTL
        case 0x9: d = static_cast<uint8_t>(t.pointer >> 8); break;  // PTH
        case 0xA: d = static_cast<uint8_t>(t.target); break;        // WAL
        case 0xB: d = static_cast<uint8_t>(t.target >> 8); break;   // WAH
        case 0xC: d = static_cast<uint8_t>(cdc_.decoderEnable << 7); break;  // STAT0: CRC OK
        case 0xD: d = 0; break;                                              // STAT1
        case 0xE: d = static_cast<uint8_t>(cdc_.decoderForm << 2 | cdc_.decoderMode << 3); break;  // STAT2
        case 0xF:  // STAT3
            d = static_cast<uint8_t>(int{!cdc_.decoderValid} << 7);
            cdc_.decoderValid = false;
            cdc_.decoderIrq.pending = false;
            cdcPoll();
            break;
        default: break;
    }
    // COMIN does not move on; STAT3 wraps round to 0.
    if (cdc_.address) cdc_.address = (cdc_.address + 1) & 0xF;
    return d;
}

void MegaCd::cdcWrite(uint8_t data) {
    Transfer& t = cdc_.transfer;
    switch (cdc_.address) {
        case 0x0:  // SBOUT
            if (cdc_.statusWait && t.busy) break;
            if (cdc_.statusRead == cdc_.statusWrite && !cdc_.statusEmpty) cdc_.statusRead = (cdc_.statusRead + 1) & 7;
            cdc_.statusFifo[cdc_.statusWrite] = data;
            cdc_.statusWrite = (cdc_.statusWrite + 1) & 7;
            cdc_.statusEmpty = false;
            cdc_.statusActive = true;
            cdc_.statusBusy = true;
            break;
        case 0x1:  // IFCTRL
            cdc_.statusEnable = data & 1;
            t.enable = (data >> 1) & 1;
            cdc_.statusWait = !((data >> 2) & 1);
            t.wait = !((data >> 3) & 1);
            cdc_.decoderIrq.enable = (data >> 5) & 1;
            cdc_.transferIrq.enable = (data >> 6) & 1;
            cdc_.commandIrq.enable = (data >> 7) & 1;
            cdcPoll();
            if (!t.enable) cdcStop();
            break;
        case 0x2: t.length = static_cast<uint16_t>((t.length & 0xF00) | data); break;
        case 0x3: t.length = static_cast<uint16_t>((t.length & 0x0FF) | (data & 0xF) << 8); break;
        case 0x4: t.source = static_cast<uint16_t>((t.source & 0xFF00) | data); break;
        case 0x5: t.source = static_cast<uint16_t>((t.source & 0x00FF) | data << 8); break;
        case 0x6: cdcStart(); break;  // DTRG
        case 0x7:                     // DTACK
            cdc_.transferIrq.pending = false;
            cdcPoll();
            break;
        case 0x8: t.target = static_cast<uint16_t>((t.target & 0xFF00) | data); break;
        case 0x9: t.target = static_cast<uint16_t>((t.target & 0x00FF) | data << 8); break;
        case 0xA:  // CTRL0
            cdc_.control = (cdc_.control & 0xFFFF00FF) | static_cast<uint32_t>(data & 0x7F) << 8;
            cdc_.decoderEnable = (data >> 7) & 1;
            cdc_.decoderMode = (cdc_.control & kMode) != 0;
            cdc_.decoderForm = (cdc_.control & kForm) && (cdc_.control & kAutoCorrection);
            break;
        case 0xB:  // CTRL1
            cdc_.control = (cdc_.control & 0xFFFFFF00) | data;
            cdc_.decoderMode = (cdc_.control & kMode) != 0;
            cdc_.decoderForm = (cdc_.control & kForm) && (cdc_.control & kAutoCorrection);
            break;
        case 0xC: t.pointer = static_cast<uint16_t>((t.pointer & 0xFF00) | data); break;
        case 0xD: t.pointer = static_cast<uint16_t>((t.pointer & 0x00FF) | data << 8); break;
        case 0xE: cdc_.control = (cdc_.control & 0xFF00FFFF) | static_cast<uint32_t>(data & 7) << 16; break;  // CTRL2
        case 0xF: {  // RESET
            cdc_.statusActive = cdc_.statusBusy = false;
            t.active = t.busy = false;
            cdc_.decoderIrq = cdc_.transferIrq = cdc_.commandIrq = {};
            cdc_.statusEnable = false;
            t.enable = false;
            cdc_.statusWait = t.wait = true;
            cdc_.control = 0;
            cdc_.decoderEnable = cdc_.decoderMode = cdc_.decoderForm = false;
            cdc_.header = {};
            cdc_.subheader = {};
            cdcStop();
            cdcPoll();
            break;
        }
        default: break;
    }
    if (cdc_.address) cdc_.address = (cdc_.address + 1) & 0xF;
}

void MegaCd::cdcDma() {
    Transfer& t = cdc_.transfer;
    if (!t.active) return;
    if (t.destination != 4 && t.destination != 5 && t.destination != 7) return;
    const uint16_t data = cdc_.ram[(t.source >> 1) & 0x1FFF];
    switch (t.destination) {
        case 4:  // PCM RAM, its active 4 KB bank
            pcmWrite(0x1000 | ((t.address >> 1) & 0xFFF) | 1, static_cast<uint8_t>(data >> 8));
            pcmWrite(0x1000 | ((t.address >> 1) & 0xFFF), static_cast<uint8_t>(data));
            t.address = (t.address + 2) & 0x7FFFF;
            break;
        case 5:  // program RAM
            subWrite(t.address & 0x7FFFE, data, true, true);
            break;
        case 7:  // word RAM
            if (!io_.wramMode) subWrite(0x080000 | (t.address & 0x3FFFE), data, true, true);
            else subWrite(0x0C0000 | (t.address & 0x1FFFE), data, true, true);
            break;
        default: break;
    }
    t.address = (t.address + 2) & 0x7FFFF;
    t.source = static_cast<uint16_t>(t.source + 2);
    for (int i = 0; i < 2; ++i) {
        const bool done = (t.length & 0xFFF) == 0;
        t.length = static_cast<uint16_t>((t.length - 1) & 0xFFF);
        if (done) cdcComplete();
    }
}

uint16_t MegaCd::cdcTransferRead() {
    Transfer& t = cdc_.transfer;
    if (!t.ready) return 0xFFFF;
    const uint16_t data = cdc_.ram[(t.source >> 1) & 0x1FFF];
    t.source = static_cast<uint16_t>(t.source + 2);
    for (int i = 0; i < 2; ++i) {
        const bool done = (t.length & 0xFFF) == 0;
        t.length = static_cast<uint16_t>((t.length - 1) & 0xFFF);
        if (done) cdcComplete();
    }
    updateIrq();
    return data;
}

void MegaCd::cdcStart() {
    Transfer& t = cdc_.transfer;
    if (!t.enable) return;
    t.active = t.busy = true;
    t.ready = t.destination == 2 || t.destination == 3;
    t.completed = false;
    cdc_.transferIrq.pending = false;
    cdcPoll();
}

void MegaCd::cdcComplete() {
    Transfer& t = cdc_.transfer;
    t.active = t.busy = t.ready = false;
    t.completed = true;
    cdc_.transferIrq.pending = true;
    cdcPoll();
}

void MegaCd::cdcStop() {
    Transfer& t = cdc_.transfer;
    t.active = t.busy = t.ready = false;
}

void MegaCd::cdcPower() { cdc_ = Cdc{}; }

// ---------------------------------------------------------------------------------
// CDD: the drive's microcontroller, emulated at the level of its commands.

int MegaCd::trackAt(int32_t lba) const {
    if (!disc_) return 0;
    const int i = disc_->trackAt(lba);
    return i < 0 ? 0 : disc_->tracks()[static_cast<size_t>(i)].number;
}

bool MegaCd::trackAudio(int track) const {
    const CdImage::Track* t = disc_ ? disc_->track(track) : nullptr;
    return t && t->audio;
}

bool MegaCd::trackData(int track) const {
    const CdImage::Track* t = disc_ ? disc_->track(track) : nullptr;
    return t && !t->audio;
}

void MegaCd::cachedSector(int32_t lba) {
    if (lba == cachedLba_) return;
    cachedLba_ = lba;
    if (disc_) disc_->readSector(lba, sector_.data());
    else sector_.fill(0);
}

double MegaCd::cddPosition(int32_t sector) const {
    // Sector number to how far out on the disc it is, for the time a seek takes.
    const double sectors = 7500.0 + 330000.0 + 6750.0;
    const double radius = 0.058 - 0.024;
    const double inner = 0.024 * 0.024, outer = 0.058 * 0.058;
    const double natural = sector + 7500.0 + CdImage::kPregap;
    return std::sqrt(std::max(0.0, natural / sectors * (outer - inner) + inner)) / radius;
}

void MegaCd::cddClock() {
    if (++cdd_.counter < 434) return;
    cdd_.counter = 0;
    // 75 times a second.
    if (!cdd_.hostClockEnable) return;
    if (cdd_.statusPending) {
        cdd_.statusPending = false;
        cdd_.irq.raise();
    }
    switch (cdd_.status) {
        case kStopped:
            cdd_.status = disc_ ? kReadingToc : kNoDisc;
            cdd_.sector = 0;
            cdd_.sample = 0;
            cdd_.track = 0;
            break;
        case kReadingToc:
            ++cdd_.sector;
            if (cdd_.sector > kLeadInEnd || cdd_.sector < kLeadInStart) {
                cdd_.status = kPaused;
                if (const int t = trackAt(cdd_.sector)) cdd_.track = t;
                cdd_.tocRead = true;
            }
            break;
        case kPlaying:
            cddReadSubcode();
            if (trackAudio(cdd_.track)) break;
            cdcDecode(cdd_.sector);
            cddAdvance();
            break;
        case kTracking:
        case kSeeking:
            if (cdd_.latency && --cdd_.latency) break;
            cdd_.status = cdd_.seeking;
            if (const int t = trackAt(cdd_.sector)) cdd_.track = t;
            break;
        default: break;
    }
}

void MegaCd::cddAdvance() {
    if (const int t = trackAt(cdd_.sector + 1)) {
        cdd_.track = t;
        ++cdd_.sector;
        cdd_.sample = 0;
        return;
    }
    if (cdd_.sector + 1 < 0) {
        cdd_.status = kLeadIn;
        cdd_.track = 0xA0;
    } else {
        cdd_.status = kLeadOut;
        cdd_.track = 0xAA;
    }
}

void MegaCd::cddSample() {
    int left = 0, right = 0;
    if (cdd_.status == kPlaying && trackAudio(cdd_.track)) {
        cachedSector(cdd_.sector);
        const uint8_t* p = &sector_[cdd_.sample];
        left = static_cast<int16_t>(p[0] | p[1] << 8);
        right = static_cast<int16_t>(p[2] | p[3] << 8);
        cdd_.sample = static_cast<uint16_t>(cdd_.sample + 4);
        if (cdd_.sample >= 2352) cddAdvance();
    }
    // The DAC: the attenuator glides to its new level one step per sample.
    left = (left * cdd_.attenuated) / 0x4000 >> 1;
    right = (right * cdd_.attenuated) / 0x4000 >> 1;
    if (cdd_.attenuated < cdd_.attenuator) ++cdd_.attenuated;
    else if (cdd_.attenuated > cdd_.attenuator) --cdd_.attenuated;
    out_.cdda.push(left, right, out_.cddaOut);
}

void MegaCd::cddReadSubcode() {
    // Images carry no subchannel data: the buffer moves on (with nothing in it) and the
    // interrupt comes as on the real thing.
    cdd_.subcodePosition = static_cast<uint8_t>((cdd_.subcodePosition + 49) & 63);
    uint8_t index = cdd_.subcodePosition;
    for (int i = 0; i < 48; ++i) cdd_.subcode[index++ & 63] = 0;
    subcodeIrq_.raise();
}

void MegaCd::cddProcess() {
    auto& st = cdd_.statusRegs;
    const auto& cmd = cdd_.commandRegs;
    auto putMsf = [&](int32_t frames) {
        int m, s, f;
        msf(frames, m, s, f);
        st[2] = static_cast<uint8_t>(m / 10);
        st[3] = static_cast<uint8_t>(m % 10);
        st[4] = static_cast<uint8_t>(s / 10);
        st[5] = static_cast<uint8_t>(s % 10);
        st[6] = static_cast<uint8_t>(f / 10);
        st[7] = static_cast<uint8_t>(f % 10);
    };
    auto clear = [&](uint8_t first) {
        st[1] = first;
        for (int i = 2; i <= 8; ++i) st[static_cast<size_t>(i)] = 0;
    };
    auto seek = [&](uint8_t then) {
        const int m = cmd[2] * 10 + cmd[3], s = cmd[4] * 10 + cmd[5], f = cmd[6] * 10 + cmd[7];
        const int32_t lba = (m * 60 + s) * 75 + f - CdImage::kPregap - 3;
        cdd_.counter = 0;
        cdd_.status = kSeeking;
        cdd_.seeking = then;
        cdd_.latency = static_cast<uint16_t>(11 + 112.5 * std::abs(cddPosition(cdd_.sector) - cddPosition(lba)));
        cdd_.sector = lba;
        cdd_.sample = 0;
        clear(0xF);
    };

    uint8_t sum = 0;
    for (int i = 0; i < 9; ++i) sum = static_cast<uint8_t>(sum + cmd[static_cast<size_t>(i)]);
    if (static_cast<uint8_t>(~sum & 0xF) != cmd[9]) {
        cdd_.status = kChecksumError;
    } else {
        switch (cmd[0]) {
            case 0x0:  // idle
                if (!cdd_.latency && st[1] == 0xF) {
                    st[1] = 0x2;
                    st[2] = static_cast<uint8_t>(cdd_.track / 10);
                    st[3] = static_cast<uint8_t>(cdd_.track % 10);
                }
                break;
            case 0x1:  // stop
                cdd_.status = disc_ ? kStopped : kNoDisc;
                clear(0);
                break;
            case 0x2:  // report
                switch (cmd[3]) {
                    case 0x0:  // absolute time
                        st[1] = cmd[3];
                        putMsf(cdd_.sector + CdImage::kPregap);
                        st[8] = static_cast<uint8_t>(trackData(cdd_.track) << 2);
                        break;
                    case 0x1: {  // time in the track
                        const CdImage::Track* t = disc_ ? disc_->track(cdd_.track) : nullptr;
                        st[1] = cmd[3];
                        putMsf(cdd_.sector - (t ? t->start : 0));
                        st[8] = static_cast<uint8_t>(trackData(cdd_.track) << 2);
                        break;
                    }
                    case 0x2:  // track number
                        clear(cmd[3]);
                        st[2] = static_cast<uint8_t>(cdd_.track / 10);
                        st[3] = static_cast<uint8_t>(cdd_.track % 10);
                        if (cdd_.sector >= kLeadInStart && cdd_.sector <= kLeadInEnd) st[2] = st[3] = 0;
                        if (disc_ && cdd_.sector >= disc_->leadOut() && cdd_.sector < disc_->leadOut() + kLeadOutSectors)
                            st[2] = st[3] = 0xA;
                        break;
                    case 0x3:  // disc length
                        st[1] = cmd[3];
                        putMsf((disc_ ? disc_->leadOut() : 0) + CdImage::kPregap);
                        st[8] = 0;
                        break;
                    case 0x4: {  // first and last tracks
                        const int first = disc_ ? disc_->firstTrack() : 0, last = disc_ ? disc_->lastTrack() : 0;
                        clear(cmd[3]);
                        st[2] = static_cast<uint8_t>(first / 10);
                        st[3] = static_cast<uint8_t>(first % 10);
                        st[4] = static_cast<uint8_t>(last / 10);
                        st[5] = static_cast<uint8_t>(last % 10);
                        break;
                    }
                    case 0x5: {  // where a track starts
                        if (cmd[4] > 9 || cmd[5] > 9) break;
                        const int number = cmd[4] * 10 + cmd[5];
                        const CdImage::Track* t = disc_ ? disc_->track(number) : nullptr;
                        st[1] = cmd[3];
                        putMsf((t ? t->start : -CdImage::kPregap) + CdImage::kPregap);
                        if (t && !t->audio) st[6] |= 8;
                        st[8] = static_cast<uint8_t>(number % 10);
                        break;
                    }
                    case 0x6: clear(cmd[3]); break;  // errors: never any
                    default: break;
                }
                break;
            case 0x3: seek(kPlaying); break;  // seek and play
            case 0x4: seek(kPaused); break;   // seek and pause
            case 0x6:                         // pause
                if (cdd_.status == kSeeking) cdd_.seeking = kPaused;
                else cdd_.status = kPaused;
                break;
            case 0x7:  // play
                if (cdd_.status == kSeeking) cdd_.seeking = kPlaying;
                else cdd_.status = kPlaying;
                break;
            case 0xA:  // move the laser some tracks: the BIOS seeks afterwards anyway
                cdd_.counter = 0;
                cdd_.status = kTracking;
                cdd_.seeking = kPaused;
                cdd_.latency = 0;
                cdd_.sample = 0;
                clear(0xF);
                break;
            default: cdd_.status = kCommandError; break;
        }
    }
    st[0] = cdd_.status;
    cddChecksum();
    cdd_.statusPending = true;
}

void MegaCd::cddChecksum() {
    uint8_t sum = 0;
    for (int i = 0; i < 9; ++i) sum = static_cast<uint8_t>(sum + cdd_.statusRegs[static_cast<size_t>(i)]);
    cdd_.statusRegs[9] = static_cast<uint8_t>(~sum & 0xF);
}

void MegaCd::cddInsert() {
    if (!disc_ || !disc_->loaded()) {
        cdd_.status = kNoDisc;
        return;
    }
    cdd_.status = kReadingToc;
    cdd_.sector = kLeadInStart;
    cdd_.sample = 0;
    cdd_.track = 0;
}

void MegaCd::cddPower() {
    cdd_ = Cdd{};
    cddInsert();
    cddChecksum();
}

// ---------------------------------------------------------------------------------
// GPU: draws rotated and scaled stamps into word RAM, one line at a time.

uint8_t MegaCd::gpuRead(uint32_t nibble) const {
    const int shift = 12 - static_cast<int>((nibble & 3) << 2);
    return static_cast<uint8_t>((wram_[(nibble >> 2) & 0x1FFFF] >> shift) & 0xF);
}

void MegaCd::gpuWrite(uint32_t nibble, uint8_t data) {
    const int shift = 12 - static_cast<int>((nibble & 3) << 2);
    uint16_t& w = wram_[(nibble >> 2) & 0x1FFFF];
    w = static_cast<uint16_t>((w & ~(0xF << shift)) | (data & 0xF) << shift);
}

void MegaCd::gpuStep(int clocks) {
    if (!gpu_.active) return;
    gpu_.counter += static_cast<uint32_t>(clocks);
    while (gpu_.counter >= gpu_.period) {
        gpu_.counter -= gpu_.period;
        gpuRender(gpu_.imageAddress, gpu_.imageHdots);
        gpu_.imageAddress = (gpu_.imageAddress + 8) & 0x7FFFF;
        if (!--gpu_.imageVdots) {
            gpu_.active = false;
            gpu_.irq.raise();
            break;
        }
    }
}

void MegaCd::gpuRender(uint32_t address, int width) {
    int stampShift = 4;
    int mapShift = 4 << (gpu_.mapSize ? 1 : 0);
    uint32_t indexMask = 0x7FF;
    uint32_t pixelMask = 0x0F;
    if (gpu_.tileSize) {
        ++stampShift;
        --mapShift;
        indexMask &= ~3u;
        pixelMask |= 0x10;
    }
    const uint32_t imageWidth = static_cast<uint32_t>(gpu_.imageVcells + 1) << 6;
    const uint32_t mapMask = gpu_.mapSize ? 0x7FFFFF : 0x07FFFF;

    uint32_t x = static_cast<uint32_t>(wram_[gpu_.vectorAddress++ & 0x1FFFF]) << 8;  // 13.3 to 13.11
    uint32_t y = static_cast<uint32_t>(wram_[gpu_.vectorAddress++ & 0x1FFFF]) << 8;
    const int16_t xstep = static_cast<int16_t>(wram_[gpu_.vectorAddress++ & 0x1FFFF]);
    const int16_t ystep = static_cast<int16_t>(wram_[gpu_.vectorAddress++ & 0x1FFFF]);
    gpu_.vectorAddress &= 0x1FFFF;

    while (width--) {
        if (gpu_.repeat) {
            x &= mapMask;
            y &= mapMask;
        }
        uint8_t output = 0;
        if (((x | y) & ~mapMask & 0xFFFFFF) == 0) {
            uint32_t xt = x >> 11, yt = y >> 11;
            const uint32_t xs = xt >> stampShift, ys = yt >> stampShift;
            const uint16_t entry = wram_[(gpu_.mapAddress + (ys << mapShift) + xs) & 0x1FFFF];
            const uint32_t index = entry & indexMask;
            if (index) {
                if (entry & 0x8000) xt = ~xt;  // horizontal flip
                if (entry & 0x4000) {          // 180 degrees
                    xt = ~xt;
                    yt = ~yt;
                }
                if (entry & 0x2000) {  // 90 degrees
                    const uint32_t t = xt;
                    xt = ~yt;
                    yt = t;
                }
                const uint32_t xp = xt & pixelMask, yp = yt & pixelMask;
                output = gpuRead((index << 8 | (xp & ~7u) << stampShift | yp << 3 | (xp & 7)) & 0x7FFFF);
            }
        }
        const uint8_t input = gpuRead(address);
        switch (io_.wramPriority) {
            case 1: output = input ? input : output; break;
            case 2: output = output ? output : input; break;
            case 3: output = input; break;
            default: break;
        }
        gpuWrite(address, output);
        address = (address + 1) & 0x7FFFF;
        if (!(address & 7)) address = (address + imageWidth - 8) & 0x7FFFF;
        x = (x + static_cast<uint32_t>(static_cast<int32_t>(xstep))) & 0xFFFFFF;
        y = (y + static_cast<uint32_t>(static_cast<int32_t>(ystep))) & 0xFFFFFF;
    }
}

void MegaCd::gpuStart() {
    if (io_.wramMode) return;  // only in 2 Mbit mode
    gpu_.active = true;
    gpu_.period = 5u * gpu_.imageHdots;
    if (gpu_.period == 0) gpu_.period = 5;
    gpu_.counter = 0;
    gpu_.imageAddress = ((gpu_.imageBase << 1) + gpu_.imageOffset) & 0x7FFFF;
    gpu_.vectorAddress = gpu_.vectorBase >> 1;
    gpu_.mapAddress = gpu_.mapBase >> 1;
    if (!gpu_.mapSize && !gpu_.tileSize) gpu_.mapAddress &= 0x1FF00;
    if (!gpu_.mapSize && gpu_.tileSize) gpu_.mapAddress &= 0x1FFC0;
    if (gpu_.mapSize && !gpu_.tileSize) gpu_.mapAddress &= 0x10000;
    if (gpu_.mapSize && gpu_.tileSize) gpu_.mapAddress &= 0x1C000;
}

// ---------------------------------------------------------------------------------
// PCM: Ricoh RF5C164, eight channels of 8-bit samples.

void MegaCd::pcmClock() {
    int left = 0, right = 0;
    if (pcm_.enable) {
        for (PcmChannel& c : pcm_.channels) {
            if (!c.enable) continue;
            int data = pcm_.ram[(c.address >> 11) & 0xFFFF];
            if (data == 0xFF) {  // loop
                c.address = static_cast<uint32_t>(c.loop) << 11;
                data = pcm_.ram[c.loop];
                if (data == 0xFF) continue;
            } else {
                c.address = (c.address + c.step) & 0x7FFFFFF;
            }
            data = (data & 0x80) ? (data & 0x7F) : -(data & 0x7F);
            left += data * c.envelope * (c.pan & 0xF) >> 5;
            right += data * c.envelope * (c.pan >> 4) >> 5;
        }
    }
    // A 10-bit DAC.
    left = (clamp16(left) >> 2) & ~15;
    right = (clamp16(right) >> 2) & ~15;
    out_.pcm.push(left, right, out_.pcmOut);
}

uint8_t MegaCd::pcmRead(uint32_t addr) {
    if (addr >= 0x1000) return pcm_.ram[static_cast<uint32_t>(pcm_.bank) << 12 | (addr & 0xFFF)];
    if (addr >= 0x10 && addr < 0x20) {
        const uint16_t offset = static_cast<uint16_t>(pcm_.channels[(addr >> 1) & 7].address >> 11);
        return static_cast<uint8_t>((addr & 1) ? offset >> 8 : offset);
    }
    return 0xFF;
}

void MegaCd::pcmWrite(uint32_t addr, uint8_t data) {
    if (addr >= 0x1000) {
        pcm_.ram[static_cast<uint32_t>(pcm_.bank) << 12 | (addr & 0xFFF)] = data;
        return;
    }
    PcmChannel& c = pcm_.channels[pcm_.channel];
    switch (addr) {
        case 0x0: c.envelope = data; break;
        case 0x1: c.pan = data; break;
        case 0x2: c.step = static_cast<uint16_t>((c.step & 0xFF00) | data); break;
        case 0x3: c.step = static_cast<uint16_t>((c.step & 0x00FF) | data << 8); break;
        case 0x4: c.loop = static_cast<uint16_t>((c.loop & 0xFF00) | data); break;
        case 0x5: c.loop = static_cast<uint16_t>((c.loop & 0x00FF) | data << 8); break;
        case 0x6:
            c.start = data;
            if (!c.enable) c.address = static_cast<uint32_t>(c.start) << 19;
            break;
        case 0x7:
            if (data & 0x40) pcm_.channel = data & 7;
            else pcm_.bank = data & 0xF;
            pcm_.enable = (data & 0x80) != 0;
            break;
        case 0x8:
            for (PcmChannel& ch : pcm_.channels) {
                ch.enable = !(data & 1);
                data >>= 1;
                if (!ch.enable) ch.address = static_cast<uint32_t>(ch.start) << 19;
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------
// Sound output

void MegaCd::Upsampler::push(int l, int r, std::vector<int16_t>& out) {
    while (next <= 1.0) {
        out.push_back(static_cast<int16_t>(clamp16(static_cast<int>(std::lround(prevL + (l - prevL) * next)))));
        out.push_back(static_cast<int16_t>(clamp16(static_cast<int>(std::lround(prevR + (r - prevR) * next)))));
        next += ratio;
    }
    next -= 1.0;
    prevL = l;
    prevR = r;
}

void MegaCd::setSampleRate(double hz) {
    out_.pcm.ratio = kSubClock / 384.0 / hz;
    out_.cdda.ratio = 44100.0 / hz;
}

std::vector<int16_t>& MegaCd::samples() {
    const size_t n = std::min(out_.pcmOut.size(), out_.cddaOut.size()) & ~size_t{1};
    for (size_t i = 0; i < n; ++i)
        out_.mixed.push_back(static_cast<int16_t>(clamp16(out_.pcmOut[i] + out_.cddaOut[i])));
    out_.pcmOut.erase(out_.pcmOut.begin(), out_.pcmOut.begin() + static_cast<long>(n));
    out_.cddaOut.erase(out_.cddaOut.begin(), out_.cddaOut.begin() + static_cast<long>(n));
    // The two cover the same time; keep any rounding drift from piling up.
    if (out_.pcmOut.size() > 4096) out_.pcmOut.clear();
    if (out_.cddaOut.size() > 4096) out_.cddaOut.clear();
    return out_.mixed;
}

}  // namespace plumsg
