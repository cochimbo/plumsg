#include "megadrive.h"

#include "i18n.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

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
        if (svp_ && svp_->dmaRead(addr & 0xFFFFFE, w)) return w;
        return read16(addr & 0xFFFFFE);
    });
    applyRegion();
    powerCycle();
}

namespace {

// A Mega CD BIOS image, or a program made to stand in for one (KRIKzz's mcd-verificator):
// "BOOT ROM" in the title and a "BR" serial. Loaded as a cartridge it goes in the Mega
// CD in place of its BIOS, with no cartridge, and a .cue of the same name, if there is
// one, in the tray.
bool isMegaCdBootRom(const std::vector<uint8_t>& rom) {
    if (rom.size() < 0x200) return false;
    const std::string title(rom.begin() + 0x120, rom.begin() + 0x150), serial(rom.begin() + 0x180, rom.begin() + 0x183);
    return title.find("BOOT ROM") != std::string::npos && serial == "BR ";
}

}  // namespace

bool MegaDrive::loadRom(const std::string& path, std::string* error) {
    if (detectSystem(path) == ConsoleSystem::MegaCD) return loadDisc(path, error);
    Cartridge loaded;
    if (!loaded.loadFile(path, error)) return false;
    cart_ = std::move(loaded);
    smsFile_ = detectSystem(path) == ConsoleSystem::MasterSystem;
    ggFile_ = detectSystem(path) == ConsoleSystem::GameGear;
    loadNotice_.clear();
    std::unique_ptr<CdImage> disc;
    const std::string cue = detectSystemFromData(cart_.rom()) == ConsoleSystem::MegaDrive ? msuMdCue(path) : "";
    if (!cue.empty()) {
        std::string why;
        auto open = std::make_unique<CdImage>();
        if (!open->open(cue, &why))
            loadNotice_ = trf("No se pudo abrir %s (%s): el juego suena sin su música en CD.",
                              std::filesystem::u8path(cue).filename().u8string().c_str(), why.c_str());
        else if (!isMegaCdBootRom(cart_.rom()) &&
                 std::all_of(cdBios_.begin(), cdBios_.end(), [](const auto& b) { return b.size() != 0x20000; }))
            loadNotice_ = tr("La música en CD de este juego (MSU-MD) necesita una BIOS del Mega CD: elígela en Consola > "
                             "BIOS del Mega CD.");
        else
            disc = std::move(open);
    }
    attachCd(std::move(disc), true);
    if (paprium_) {
        // Its music, recordings the cartridge plays, from a folder by the ROM with its name.
        const std::filesystem::path rom = std::filesystem::u8path(path);
        const std::filesystem::path folder = rom.parent_path() / rom.stem();
        if (paprium_->setMusicFolder(folder) == 0)
            loadNotice_ = trf("La música de Paprium no está en la ROM: pon sus pistas (.ogg, .mp3 o .wav, cada una "
                              "empezando por su número en el álbum) en la carpeta %s.",
                              folder.u8string().c_str());
    }
    return true;
}

void MegaDrive::loadRomData(std::vector<uint8_t> rom) {
    cart_.loadData(std::move(rom));
    smsFile_ = ggFile_ = false;
    loadNotice_.clear();
    attachCd(nullptr, true);
}

void MegaDrive::loadRomDataWithDisc(std::vector<uint8_t> rom, std::unique_ptr<CdImage> disc) {
    cart_.loadData(std::move(rom));
    smsFile_ = ggFile_ = false;
    loadNotice_.clear();
    attachCd(std::move(disc), true);
}

// Puts the disc in (none for a cartridge on its own) and starts the console afresh.
void MegaDrive::attachCd(std::unique_ptr<CdImage> disc, bool cartridge) {
    mcd_.reset();
    disc_ = std::move(disc);
    bootRomAsBios_ = cartridge && isMegaCdBootRom(cart_.rom());
    if (disc_ || bootRomAsBios_) {
        mcd_ = std::make_unique<MegaCd>();
        mcd_->setCartridgeMode(cartridge && !bootRomAsBios_);
        mcd_->insert(disc_.get());
        if (bootRomAsBios_) mcd_->setBios(cart_.rom());
    }
    romOriginal_.clear();
    ramCheats_.clear();
    setUpMars();
    setUpSms();
    setUpPaprium();
    setUpSvp();
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
    smsFile_ = ggFile_ = false;
    discRegion_ = wanted;
    loadNotice_.clear();
    attachCd(std::move(disc), false);
    return true;
}

std::vector<int16_t>* MegaDrive::addOnSamples() {
    if (mars_) return &mars_->samples();
    if (mcd_) return &mcd_->samples();
    if (paprium_) return &paprium_->samples();
    return nullptr;
}

void MegaDrive::setUpMars() {
    const bool wanted = detectSystemFromData(cart_.rom()) == ConsoleSystem::Mega32X;
    if (wanted && !mars_) mars_ = std::make_unique<Mars>(cart_);
    if (!wanted) mars_.reset();
    vdp_.frame().mars.fill(0);
}

void MegaDrive::setUpPaprium() {
    paprium_.reset();
    if (!sms_ && !disc_ && Paprium::detect(cart_)) paprium_ = std::make_unique<Paprium>(cart_);
    // The chip's emulation sends every sprite's tiles each frame (the chip keeps them in
    // VRAM and sends only the new ones, which isn't known how), far more than the VDP
    // takes in a frame: done in the VDP's time the game runs at half speed with enemies on
    // screen. DMA from the chip's RAM finishes at once instead.
    vdp_.setInstantDma(paprium_ != nullptr);
}

void MegaDrive::setUpSvp() {
    svp_.reset();
    if (!sms_ && !disc_ && Svp::detect(cart_)) svp_ = std::make_unique<Svp>(cart_);
}

void MegaDrive::setUpSms() {
    const ConsoleSystem inside = detectSystemFromData(cart_.rom());
    const bool gameGear = !disc_ && (ggFile_ || (!smsFile_ && inside == ConsoleSystem::GameGear));
    const bool wanted = gameGear || (!disc_ && (smsFile_ || inside == ConsoleSystem::MasterSystem));
    if (sms_ && sms_->gameGear() != gameGear) sms_.reset();
    if (wanted && !sms_) {
        sms_ = std::make_unique<MasterSystem>(cart_, zram_, vdp_.frame(), gameGear);
        opll_ = std::make_unique<Ym2413>();
    }
    if (wanted) cart_.setMasterSystemHeader(smsRegion(cart_.rom(), gameGear), gameGear);
    if (!wanted) {
        sms_.reset();
        opll_.reset();
        vdp_.frame().aspectLines = 224;
    }
}

void MegaDrive::setRegion(Region r) {
    region_ = r;
    applyRegion();
}

void MegaDrive::applyRegion() {
    if (mcd_ && !mcd_->cartridgeMode() && !bootRomAsBios_) configureFor(discRegion_);
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
    mixer_.setGains(100, 50);
    if (sms_) {
        sms_->vdp().setPal(isPal);
        sms_->setJapanese(r == Region::JapanNtsc);
        sms_->setFmPresent(!sms_->gameGear() && (r == Region::JapanNtsc || smsFmAlways_));
        // The YM2413 stands in for the YM2612 in the mix; the PSG is the main voice here.
        opll_->setMasterClock(master);
        opll_->setVolume(sms_->fmAudible() ? 100 : 0);
        mixer_.setFmRate(opll_->sampleRate());
        mixer_.setGains(100, 100);
    }
    if (mars_) {
        mars_->setSampleRate(mixer_.outputRate(), master);
        mars_->setPal(isPal);
    }
    if (mcd_) {
        mcd_->setMasterClock(master);
        mcd_->setSampleRate(mixer_.outputRate());
        const int slot = cdBiosSlot(r);
        if (slot >= 0 && !bootRomAsBios_) mcd_->setBios(cdBios_[static_cast<size_t>(slot)]);
    }
    // Version register: bit 7 overseas, bit 6 PAL, bit 5 no expansion unit; version 0
    // (no TMSS).
    uint8_t version = mcd_ ? 0x00 : 0x20;
    if (effectiveRegion_ != Region::JapanNtsc) version |= 0x80;
    if (isPal) version |= 0x40;
    io_.setVersion(version);
}

int MegaDrive::cdBiosSlot(Region r) const {
    const int slot = r == Region::JapanNtsc ? 0 : r == Region::EuropePal ? 2 : 1;
    if (cdBios_[static_cast<size_t>(slot)].size() == 0x20000) return slot;
    // A cartridge boots by itself and does not check the BIOS's region: any will do.
    if (mcd_ && mcd_->cartridgeMode())
        for (int i = 0; i < 3; ++i)
            if (cdBios_[static_cast<size_t>(i)].size() == 0x20000) return i;
    return -1;
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
    if (svp_) svp_->powerOn();
    if (sms_) sms_->powerOn();
    z80_.powerOn();
    reset();
}

void MegaDrive::reset() {
    cart_.reset();
    if (paprium_) paprium_->reset();
    tmss_ = 1;
    loadedThumbnail_.clear();
    io_.reset();
    ym_.reset();
    ym_.samples().clear();
    if (opll_) {
        opll_->reset();
        opll_->samples().clear();
        opll_->setClockPosition(0);
    }
    psg_.reset();
    if (roland_) roland_->allNotesOff();
    psg_.setStereo(0xFF);
    mixer_.reset();
    z80_.reset();
    z80Bank_ = 0;
    z80BusReq_ = false;
    z80Reset_ = true;
    now_ = lineStart_ = m68kClock_ = z80Clock_ = psgClock_ = vdpFreeAt_ = 0;
    m68kBusStall_ = 0;
    psgRemainder_ = 0;
    refreshClocks_ = 0;
    refreshAfterFetch_ = false;
    ym_.setClockPosition(0);
    psgBuffer_.clear();
    if (mars_) {
        mars_->resetButton();  // after powerOn() (a power cycle) there is nothing to do
        mars_->rebaseClock();
        mars_->samples().clear();
    }
    if (mcd_) {
        mcd_->reset();
        mcd_->rebaseClock();
        mcd_->samples().clear();
    }
    if (svp_) svp_->reset();
    cpu_.setIrqLevel(0);
    cpu_.reset();
    if (sms_) {
        // The Z80 runs the cartridge from its first byte; there is no 68000 to wake it.
        sms_->reset();
        z80Reset_ = false;
        z80_.setIrqLine(false);
    }
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
            if (svp_) svp_->runUntil(m68kClock_);
            const uint64_t start = m68kClock_;
            m68kClock_ += m68kBusStall_;
            m68kBusStall_ = 0;
            now_ = m68kClock_;
            const uint64_t cycles = static_cast<uint64_t>(cpu_.step());
            // Overclocked, each cycle takes fewer master clocks (rounded, never 0).
            const uint64_t clocks = overclock_ == 100 ? cycles * 7
                                                      : std::max<uint64_t>(1, (cycles * 700 + overclock_ / 2) / overclock_);
            m68kClock_ += clocks;
            // The DRAM refresh comes every 128 cycles, whatever the 68000 is doing, and holds
            // its bus for 2 (testpico's "time loop" counts them). It waits for the bus
            // cycle under way to end, and the next one waits for it, unless that one is to
            // the VDP or the I/O ports, which are not on that bus. Those accesses are taken
            // as the instruction's last before the fetch of the next one; a refresh asked
            // for in the bus cycle before them, or done within them, costs nothing
            // (misc_test's "M68K ROM speed" 1, 3 and 4). Nor does one that comes while the
            // 68000 waits for something else (the VDP, a DMA, the Z80 on its bus).
            refreshClocks_ += static_cast<int>(m68kClock_ - start);
            const int ran = static_cast<int>(clocks);
            const int freeFrom = 4 * 7, freeTo = std::min(ran, freeFrom + freeAccesses_ * 4 * 7);
            freeAccesses_ = 0;
            auto stallFor = [&](int stall) {
                m68kClock_ += static_cast<uint64_t>(stall);
                refreshClocks_ += stall;
            };
            if (refreshAfterFetch_) {
                // One asked for in the last instruction's final fetch is done at this one's
                // start: free if this one starts on the VDP.
                refreshAfterFetch_ = false;
                stallFor(freeTo == ran && freeTo > freeFrom ? 0 : kRefreshStall);
            }
            while (refreshClocks_ >= kRefreshEvery) {
                refreshClocks_ -= kRefreshEvery;
                // refreshClocks_ is now how long before the instruction's end it came.
                const int before = refreshClocks_;
                if (before >= ran) continue;
                if (before < freeFrom) refreshAfterFetch_ = true;
                else if (before < freeTo) stallFor(std::max(0, kRefreshStall - (before - freeFrom)));
                else if (before >= freeTo + 4 * 7 || freeTo == freeFrom) stallFor(kRefreshStall);
            }
        } else if (z80Running()) {
            now_ = z80Clock_;
            inZ80_ = true;
            z80Clock_ += static_cast<uint64_t>(z80_.step()) * 15;
            inZ80_ = false;
        } else {
            // A stopped Z80 keeps up with the 68000, so it starts the moment it is let go.
            z80Clock_ = std::max(z80Clock_, std::min(clock, m68kClock_));
        }
    }
    now_ = clock;
    if (mars_) mars_->runUntil(clock);
    if (mcd_) mcd_->runUntil(clock);
    if (svp_) svp_->runUntil(clock);
}

void MegaDrive::syncSound() {
    if (now_ > psgClock_) {
        const uint64_t clocks = now_ - psgClock_ + static_cast<uint64_t>(psgRemainder_);
        psg_.run(static_cast<int>(clocks / 15));
        psgRemainder_ = static_cast<int>(clocks % 15);
        psgClock_ = now_;
    }
    if (opll_) opll_->runUntil(now_);
    else ym_.runUntil(now_);
}

void MegaDrive::setCheats(const std::vector<CheatPatch>& cheats) {
    // Undo in reverse, so a byte patched twice gets its true original back.
    for (auto it = romOriginal_.rbegin(); it != romOriginal_.rend(); ++it) cart_.patchRom(it->first, it->second, nullptr);
    romOriginal_.clear();
    ramCheats_.clear();
    auto patch = [&](uint32_t offset, uint8_t value) {
        uint8_t old = 0;
        if (cart_.patchRom(offset, value, &old)) romOriginal_.emplace_back(offset, old);
    };
    if (sms_) {
        // Z80 addresses: RAM from 0xC000; below it the ROM, in the bank the slot shows
        // at power on, or in every bank where the byte is what the code expects.
        const std::vector<uint8_t>& rom = cart_.rom();
        for (const CheatPatch& c : cheats) {
            if (c.address >= 0xC000) {
                ramCheats_.push_back(c);
            } else if (c.compare < 0) {
                patch(c.address, static_cast<uint8_t>(c.value));
            } else {
                const uint32_t start = c.address < 0x4000 ? c.address : c.address & 0x3FFF;
                const uint32_t step = c.address < 0x4000 ? static_cast<uint32_t>(rom.size()) : 0x4000;
                for (uint32_t o = start; o < rom.size(); o += step)
                    if (rom[o] == c.compare) patch(o, static_cast<uint8_t>(c.value));
            }
        }
        applyRamCheats();
        return;
    }
    for (const CheatPatch& c : cheats) {
        if (c.address >= 0xE00000) {
            ramCheats_.push_back(c);
            continue;
        }
        if (c.address >= 0x400000) continue;
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
    if (sms_) {
        for (const CheatPatch& c : ramCheats_) zram_[c.address & 0x1FFF] = static_cast<uint8_t>(c.value);
        return;
    }
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
    const uint32_t chip = roland_ ? roland_->chipChannels() : RolandSound::kAllVoices;
    if (chip == rolandChip_) return;
    rolandChip_ = chip;
    ym_.setMuted(0x3F & ~chip, rolandPercent_);
    psg_.setMuted(0xF & ~(chip >> 6), rolandPercent_);
    if (opll_) opll_->setMuted(RolandSound::opllChannels(RolandSound::kAllVoices & ~chip), rolandPercent_);
}

void MegaDrive::setVgmWriter(VgmWriter* writer) {
    vgm_ = writer;
    if (vgm_ && sms_) vgm_->beginSms(psg_, sms_->fmPresent() ? opll_.get() : nullptr, sms_->gameGear(), now_, masterClock());
    else if (vgm_) vgm_->begin(ym_, psg_, now_, masterClock());
}

void MegaDrive::runFrameSms() {
    VdpSms& vdp = sms_->vdp();
    if (sms_->pausePressed()) z80_.triggerNmi();
    const int lines = vdp.linesPerFrame();
    for (int line = 0; line < lines; ++line) {
        lineStart_ = now_;
        if (smsLineEarly_) smsLineEarly_ = false;  // a port access already started it
        else vdp.startLine(line, drawing_);
        smsLine_ = line;
        z80_.setIrqLine(vdp.irq());
        const uint64_t end = lineStart_ + kMasterPerLine;
        // The line counter and the frame interrupt come a few cycles into the line.
        bool lineDue = true, frameDue = true;
        auto raiseDue = [&](uint64_t upTo) {
            if (lineDue && upTo >= lineStart_ + VdpSms::kLineIrqAt * 15) {
                vdp.runLineCounter(line);
                lineDue = false;
                z80_.setIrqLine(vdp.irq());
            }
            if (frameDue && upTo >= lineStart_ + VdpSms::kFrameIrqAt * 15) {
                vdp.raiseFrameInterrupt(line);
                frameDue = false;
                z80_.setIrqLine(vdp.irq());
            }
        };
        while (z80Clock_ < end) {
            raiseDue(z80Clock_);
            now_ = z80Clock_;
            z80Clock_ += static_cast<uint64_t>(z80_.step()) * 15;
        }
        raiseDue(end);
        now_ = end;
    }
    vdp.endFrame();
}

// How far into the line a Master System port access comes. One that comes after the end
// of the line (the instruction began before it) starts the next line first, so that it
// sees that line's counters and flags.
int MegaDrive::smsPortCycle() {
    int cycle = lineClock() / 15 + z80_.ioCycle;
    if (smsLineEarly_) return cycle - VdpSms::kCyclesPerLine;
    if (cycle >= VdpSms::kCyclesPerLine && smsLine_ + 1 < sms_->vdp().linesPerFrame()) {
        sms_->vdp().startLine(smsLine_ + 1, drawing_);
        smsLineEarly_ = true;
        cycle -= VdpSms::kCyclesPerLine;
    }
    return cycle;
}

void MegaDrive::runFrame() {
    if (paprium_ && paprium_->takeRestart()) reset();
    applyRamCheats();
    if (roland_) roland_->beginFrame(now_);
    if (sms_) runFrameSms();
    else runFrameMd();
    syncSound();
    ++frames_;
    loadedThumbnail_.clear();
    // Hand this frame's sound to the mixer.
    if (paprium_) paprium_->runAudio(psg_.samples().size() / 2, mixer_.outputRate());
    psgBuffer_.insert(psgBuffer_.end(), psg_.samples().begin(), psg_.samples().end());
    psg_.clearSamples();
    const size_t before = mixer_.samples().size() / 2;
    if (sms_ && !sms_->psgAudible()) std::fill(psgBuffer_.begin(), psgBuffer_.end(), int16_t{0});
    mixer_.mix(opll_ ? opll_->samples() : ym_.samples(), psgBuffer_, addOnSamples());
    if (roland_) roland_->endFrame(now_, mixer_.output(), before, mixer_.outputRate());
}

void MegaDrive::runFrameMd() {
    const int lines = vdp_.linesPerFrame();
    for (int line = 0; line < lines; ++line) {
        lineStart_ = now_;
        vdp_.startLine(line, lineStart_);
        if (mars_) mars_->startLine(line, vdp_.activeLines());
        updateIrq();
        z80_.setIrqLine(vdp_.z80Irq());
        runUntil(lineStart_ + VdpMd::kActiveClocks);
        if (line < vdp_.activeLines() && drawing_) {
            vdp_.sync(lineStart_ + VdpMd::kActiveClocks);
            vdp_.renderLine(line);
            if (mars_) mars_->renderLine(line, vdp_.frame());
        }
        if (mars_) mars_->startHblank(line, vdp_.activeLines());
        vdp_.endLine(line);
        updateIrq();
        if (line == vdp_.activeLines()) {
            runUntil(lineStart_ + VdpMd::kVcountClocks + VdpMd::kVintDelay);
            vdp_.raiseVint();
            updateIrq();
            z80_.setIrqLine(vdp_.z80Irq());
        }
        runUntil(lineStart_ + kMasterPerLine);
    }
    vdp_.endFrame();
}

bool MegaDrive::runAhead(int frames) {
    if (frames <= 0) return true;
    if (!saveState(aheadState_, true)) return false;
    // The frames to come are run again for real, heard and recorded then.
    RolandSound* roland = roland_;
    VgmWriter* vgm = vgm_;
    roland_ = nullptr;
    vgm_ = nullptr;
    const bool drawing = drawing_;
    for (int i = 1; i <= frames; ++i) {
        drawing_ = i == frames;  // only the last picture is seen
        runFrame();
    }
    drawing_ = drawing;
    roland_ = roland;
    vgm_ = vgm;
    // The picture is not part of the state, so loading it keeps the one just drawn.
    std::string error;
    return loadState(aheadState_, &error);
}

void MegaDrive::irqAcknowledge(int level) {
    vdp_.acknowledge(level);
    updateIrq();
}

// ---------------------------------------------------------------------------------
// 68000 memory map

uint16_t MegaDrive::readVdp(uint32_t addr) {
    if (!inZ80_) ++freeAccesses_;
    const uint64_t t = vdpClock();
    switch (addr & 0x1F) {
        case 0x00: case 0x02: {
            int wait = 0;
            const uint16_t v = vdp_.readData(t, &wait);
            // Whoever reads waits for the word: the 68000, or the Z80 through its window.
            (inZ80_ ? z80Clock_ : m68kClock_) += static_cast<uint64_t>(wait);
            vdpFreeAt_ = t + static_cast<uint64_t>(wait);
            return v;
        }
        case 0x04: case 0x06:  // bits 15-10 are not driven
            return static_cast<uint16_t>((vdp_.readStatus(static_cast<int>(t - lineStart_), t) & 0x03FF) |
                                         (openBus() & 0xFC00));
        case 0x08: case 0x0A: case 0x0C: case 0x0E: return vdp_.readHV(lineClock());
        default: return 0xFFFF;
    }
}

void MegaDrive::writeVdp(uint32_t addr, uint16_t value, bool byte) {
    const uint32_t port = addr & 0x1F;
    if (port >= 0x10 && port < 0x18) {
        if (!byte || (addr & 1)) psgWrite(static_cast<uint8_t>(value));
        return;
    }
    if (!inZ80_) ++freeAccesses_;
    // Byte writes reach the VDP with the byte on both halves of the bus.
    if (byte) value = static_cast<uint16_t>((value & 0xFF) << 8 | (value & 0xFF));
    const uint64_t t = vdpClock();
    int stall = 0;
    if (port < 0x04) stall = vdp_.writeData(value, t);
    else if (port < 0x08) {
        stall = vdp_.writeControl(value, t);
        if (stall > 0) dmaBusUntil_ = t + static_cast<uint64_t>(stall);  // only a DMA stalls here
    }
    m68kClock_ += static_cast<uint64_t>(stall);
    vdpFreeAt_ = t + static_cast<uint64_t>(stall);
    updateIrq();
}

uint8_t MegaDrive::readIo(uint32_t addr) {
    if (!inZ80_) ++freeAccesses_;
    return io_.read(addr | 1, now_);
}

void MegaDrive::writeIo(uint32_t addr, uint8_t value) {
    if (!inZ80_) ++freeAccesses_;
    io_.write(addr | 1, value, now_);
}

// The 68000 waits a cycle for the Z80's side of the bus (misc_test's "M68K ROM speed 5",
// a loop reading the YM2612).
uint8_t MegaDrive::readZ80Area(uint32_t addr) {
    m68kClock_ += kM68kZ80AreaWait;
    if (!z80BusFree()) return 0xFF;
    return read(static_cast<uint16_t>(addr & 0x7FFF));
}

void MegaDrive::writeZ80Area(uint32_t addr, uint8_t value) {
    m68kClock_ += kM68kZ80AreaWait;
    if (!z80BusFree()) return;
    fromM68k_ = true;
    write(static_cast<uint16_t>(addr & 0x7FFF), value);
    fromM68k_ = false;
}

void MegaDrive::setMemoryWatch(MemoryWatch watch) {
    watch_ = std::move(watch);
    watching_ = watch_ || lockCount_ > 0;
}

void MegaDrive::lockSoundRequests(uint32_t addr, int count, bool all) {
    lockAddr_ = addr;
    lockCount_ = count;
    lockAll_ = all;
    watching_ = watch_ || lockCount_ > 0;
}

void MegaDrive::poke(uint32_t addr, uint8_t value) {
    if (sms_ && addr >= 0xC000 && addr < 0x10000)
        zram_[addr & 0x1FFF] = value;
    else if (addr >= 0xFF0000)
        ram_[addr & 0xFFFF] = value;
    else if (addr >= 0xA00000 && addr < 0xA04000)
        zram_[addr & 0x1FFF] = value;
}

bool MegaDrive::watchWrite(uint32_t addr, uint8_t value, bool m68k) {
    if (m68k && (lockAll_ || (value != 0 && value != 0x80)) && addr - lockAddr_ < static_cast<uint32_t>(lockCount_)) return false;
    if (watch_) watch_(addr, value, m68k);
    return true;
}

void MegaDrive::writeBusControl(uint32_t addr, bool set) {
    if ((addr & 0xFFFF00) == 0xA11100) {
        if (set && !z80BusReq_) {
            z80Clock_ = std::max(z80Clock_, now_);
            // A running Z80 lets go at the end of the machine cycle under way: till then
            // BUSACK still reads 1 and its RAM is not the 68000's (misc_test's "Z80
            // BUSREQ delay flag" and "delay RAM read").
            z80BusGrantAt_ = z80Reset_ ? now_ : std::min(z80Clock_, now_ + kZ80BusGrantDelay);
        }
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
    if (paprium_ && Paprium::inWindow(addr)) return paprium_->read8(addr);
    if (svp_) {
        uint16_t w = 0;
        if (svp_->read68k(addr & ~1u, w)) return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
    if (addr < 0x400000) return cart_.read8(addr);
    // The cartridge slot of a Mega CD with no cartridge: no RAM cartridge either, whose
    // size code (0x400001, 0-6) is how software tells.
    if (addr < 0x800000 && mcd_) return 0xFF;
    if (addr >= 0xE00000) return ram_[addr & 0xFFFF];
    if (paprium_ && addr == 0xA14101) return tmss_;
    if (addr >= 0xC00000) {
        if (addr >= 0xC00020) return 0xFF;
        const uint16_t w = readVdp(addr & ~1u);
        return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
    if (addr < 0xA10000 && addr >= 0xA00000) return readZ80Area(addr);
    if (addr >= 0xA10000 && addr < 0xA10020) return readIo(addr);
    if (addr == 0xA11100) return static_cast<uint8_t>((openBus() >> 8 & 0xFE) | (z80BusAcked() ? 0x00 : 0x01));
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
    if (paprium_ && Paprium::inWindow(addr)) return paprium_->read16(addr);
    if (svp_) {
        uint16_t w = 0;
        if (svp_->read68k(addr, w)) return w;
    }
    if (addr < 0x400000) return cart_.read16(addr);
    if (addr < 0x800000 && mcd_) return 0xFFFF;
    if (addr >= 0xE00000) return static_cast<uint16_t>(ram_[addr & 0xFFFF] << 8 | ram_[(addr & 0xFFFF) + 1]);
    if (addr >= 0xC00000) return addr < 0xC00020 ? readVdp(addr) : 0xFFFF;
    if (addr < 0xA10000 && addr >= 0xA00000) {
        const uint8_t b = readZ80Area(addr);
        return static_cast<uint16_t>(b << 8 | b);
    }
    if (addr >= 0xA10000 && addr < 0xA10020) {
        const uint8_t b = readIo(addr);
        return static_cast<uint16_t>(b << 8 | b);
    }
    if (addr == 0xA11100) return static_cast<uint16_t>((openBus() & 0xFEFF) | (z80BusAcked() ? 0x0000 : 0x0100));
    return 0x0000;
}

// The bits of a register the hardware leaves undriven read whatever was last on the data
// bus: the 68000's prefetch of the word after the instruction. Games that test BUSACK with
// TST.B instead of BTST #0 (Indy Heat) only get past their wait through those bits.
uint16_t MegaDrive::openBus() {
    const uint32_t pc = cpu_.pc & 0xFFFFFE;
    if (pc < 0x400000 || pc >= 0xE00000) return read16(pc);
    return 0x0000;
}

void MegaDrive::write8(uint32_t addr, uint8_t value) {
    addr &= 0xFFFFFF;
    if (mars_ && mars_->write68k(addr, static_cast<uint16_t>(value << 8 | value), !(addr & 1), (addr & 1) != 0)) return;
    if (mcd_ && mcd_->write68k(addr & ~1u, static_cast<uint16_t>(value << 8 | value), !(addr & 1), (addr & 1) != 0)) return;
    if (svp_ && svp_->write68k(addr & ~1u, static_cast<uint16_t>(value << 8 | value), !(addr & 1), (addr & 1) != 0)) return;
    if (addr >= 0xE00000) {
        if (watching_ && !watchWrite(0xFF0000 | addr, value, true)) return;
        ram_[addr & 0xFFFF] = value;
    } else if (paprium_ && Paprium::inWindow(addr)) {
        paprium_->write8(addr, value);
    } else if (addr < 0x400000) {
        cart_.write8(addr, value);
    } else if (addr >= 0xC00000) {
        if (addr < 0xC00020) writeVdp(addr, value, true);
    } else if (addr >= 0xA00000 && addr < 0xA10000) {
        writeZ80Area(addr, value);
    } else if (addr >= 0xA10000 && addr < 0xA10020) {
        writeIo(addr, value);
    } else if (addr == 0xA11100 || addr == 0xA11200) {
        writeBusControl(addr, (value & 1) != 0);
    } else if (paprium_ && (addr == 0xA14101 || (addr >= 0xA130F2 && addr < 0xA13100))) {
        if (addr == 0xA14101) tmss_ = value;  // Paprium's chip has no banks
    } else if (addr >= 0xA130F0 && addr < 0xA13100) {
        cart_.writeTimeRegister(addr, value);
    }
}

void MegaDrive::write16(uint32_t addr, uint16_t value) {
    addr &= 0xFFFFFE;
    if (mars_ && mars_->write68k(addr, value, true, true)) return;
    if (mcd_ && mcd_->write68k(addr, value, true, true)) return;
    if (svp_ && svp_->write68k(addr, value, true, true)) return;
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
    } else if (paprium_ && Paprium::inWindow(addr)) {
        paprium_->write16(addr, value);
    } else if (addr < 0x400000) {
        cart_.write16(addr, value);
    } else if (addr >= 0xC00000) {
        if (addr < 0xC00020) writeVdp(addr, value, false);
    } else if (addr >= 0xA00000 && addr < 0xA10000) {
        writeZ80Area(addr, static_cast<uint8_t>(value >> 8));  // only the high byte arrives
    } else if (addr >= 0xA10000 && addr < 0xA10020) {
        writeIo(addr, static_cast<uint8_t>(value));
    } else if (addr == 0xA11100 || addr == 0xA11200) {
        writeBusControl(addr, (value & 0x0100) != 0);
    } else if (paprium_ && (addr == 0xA14100 || (addr >= 0xA130F2 && addr < 0xA13100))) {
        if (addr == 0xA14100) tmss_ = static_cast<uint8_t>(value);
    } else if (addr >= 0xA130F0 && addr < 0xA13100) {
        cart_.writeTimeRegister(addr | 1, static_cast<uint8_t>(value));
    }
}

void MegaDrive::psgWrite(uint8_t value) {
    syncSound();
    psg_.write(value);
    if (vgm_) vgm_->psg(value, now_);
    if (roland_) {
        roland_->psgWrite(psg_, now_);
        rolandChipChannels();
    }
}

// ---------------------------------------------------------------------------------
// Z80 memory map

uint8_t MegaDrive::in(uint16_t port) {
    if (!sms_) return 0xFF;
    if (sms_->fmPresent() && (port & 0xFF) == 0xF2) return sms_->fmControl();
    const uint8_t value = sms_->in(static_cast<uint8_t>(port), smsPortCycle());
    z80_.setIrqLine(sms_->vdp().irq());  // reading the status clears the interrupt
    return value;
}

void MegaDrive::out(uint16_t port, uint8_t value) {
    if (!sms_) return;
    if ((port & 0xC0) == 0x40) {
        if (watch_) watch_(0xA04000, value, true);  // who drives the sound chips
        psgWrite(value);
        return;
    }
    if (sms_->gameGear() && (port & 0xFF) == 0x06) {
        syncSound();
        psg_.setStereo(value);
        if (vgm_) vgm_->ggStereo(value, now_);
        return;
    }
    if (sms_->fmPresent() && (port & 0xFF) >= 0xF0 && (port & 0xFF) <= 0xF2) {
        syncSound();
        if ((port & 0xFF) == 0xF2) {
            sms_->setFmControl(value);
            opll_->setVolume(sms_->fmAudible() ? 100 : 0);
        } else {
            if (watch_) watch_(0xA04000, value, true);
            opll_->write(port & 1, value);
            if (vgm_) vgm_->opll(port & 1, value, now_);
            if (roland_) {
                roland_->opllWrite(port & 1, value, *opll_, now_);
                rolandChipChannels();
            }
        }
        return;
    }
    sms_->out(static_cast<uint8_t>(port), value, smsPortCycle());
    z80_.setIrqLine(sms_->vdp().irq());  // an interrupt enable may have changed
}

// A DMA from 68000 memory keeps that bus till it ends: the Z80 going through its bank
// waits (misc_test's "Z80 halt when read ROM while DMA").
void MegaDrive::waitForDmaBus() {
    if (inZ80_ && z80Clock_ < dmaBusUntil_) z80Clock_ = dmaBusUntil_;
}

uint8_t MegaDrive::read(uint16_t addr) {
    if (sms_) return sms_->read(addr);
    if (addr < 0x4000) return zram_[addr & 0x1FFF];
    if (addr < 0x6000) {
        syncSound();
        return ym_.readStatus();
    }
    if (addr >= 0x8000) {
        // The banked window onto the 68000's address space.
        const uint32_t a = (static_cast<uint32_t>(z80Bank_) << 15) | (addr & 0x7FFF);
        waitForDmaBus();
        z80Clock_ += kZ80BankDelay;
        if (inZ80_) m68kBusStall_ += kM68kBankStall;
        if (a >= 0xA00000 && a < 0xA10000) return 0xFF;  // the Z80 cannot see itself
        // Nor read the 68000's RAM, only write it (misc_test's "Z80 write-only to M68K RAM").
        if (a >= 0xE00000) return 0xFF;
        return read8(a);
    }
    if (addr >= 0x7F00 && addr < 0x7F20) {
        z80Clock_ += kZ80VdpDelay;
        const uint16_t w = readVdp(addr & 0x1E);
        return static_cast<uint8_t>((addr & 1) ? w : w >> 8);
    }
    return 0xFF;
}

void MegaDrive::write(uint16_t addr, uint8_t value) {
    if (sms_) {
        // Watched by the program's own address (the RAM from 0xC000), all as the program's.
        if (watching_ && addr >= 0xC000 && !watchWrite(addr, value, true)) return;
        sms_->write(addr, value);
        return;
    }
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
        z80Clock_ += kZ80VdpDelay;
        if ((addr & 0x1F) >= 0x10) psgWrite(value);
    } else if (addr >= 0x8000) {
        waitForDmaBus();
        z80Clock_ += kZ80BankDelay;
        if (inZ80_) m68kBusStall_ += kM68kBankStall;
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
        StateWriter w;
        vdp_.fifoState(w);
        block("VDPF", w.data());
    }
    {
        StateWriter w;
        vdp_.satState(w);
        block("SATC", w.data());
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
        w(refreshClocks_);
        block("RFSH", w.data());
    }
    {
        StateWriter w;
        cpu_.prefetchState(w);
        block("PFCH", w.data());
    }
    {
        StateWriter w;
        w(z80BusGrantAt_);
        block("ZBGA", w.data());
    }
    {
        StateWriter w;
        io_.state(w);
        block("IO  ", w.data());
    }
    {
        StateWriter w;
        io_.riseState(w);
        block("IOTH", w.data());
    }
    {
        StateWriter w;
        w(dmaBusUntil_);
        block("DMAB", w.data());
    }
    {
        StateWriter w;
        w(m68kBusStall_, refreshAfterFetch_);
        block("BSTL", w.data());
    }
    {
        StateWriter w;
        io_.serialState(w);
        block("IOSR", w.data());
    }
    if (overclock_ != 100) {
        StateWriter w;
        uint16_t percent = static_cast<uint16_t>(overclock_);
        w(percent);
        block("OVCK", w.data());
    }
    {
        StateWriter w;
        cart_.state(w);
        block("CART", w.data());
    }
    if (cart_.hasEeprom()) {
        StateWriter w;
        cart_.eepromState(w);
        block("EEPR", w.data());
    }
    {
        StateWriter w;
        mixer_.state(w);
        block("MIX ", w.data());
    }
    if (mars_ || mcd_ || paprium_) {
        StateWriter w;
        mixer_.addOnState(w);
        block("MIXA", w.data());
    }
    if (mars_) {
        StateWriter w;
        mars_->state(w);
        block("32X ", w.data());
        StateWriter c;
        mars_->timingState(c);
        block("32X2", c.data());  // "32XC" in 0.0.27, before the boot ROMs turned the cache on
        StateWriter i;
        mars_->ioState(i);
        block("32XI", i.data());
    }
    if (mcd_) {
        StateWriter w;
        mcd_->state(w);
        block("MCD ", w.data());
    }
    if (paprium_) {
        StateWriter w;
        paprium_->state(w);
        block("PAPR", w.data());
    }
    if (svp_) {
        StateWriter w;
        svp_->state(w);
        block("SVP ", w.data());
    }
    {  // after M68K and MCD, which settle it for older states
        StateWriter w;
        cpu_.irqState(w);
        if (mcd_) mcd_->sub().irqState(w);
        block("IRQS", w.data());
    }
    if (sms_) {
        StateWriter w;
        sms_->state(w);
        block("SMS ", w.data());
    }
    if (opll_) {
        std::vector<uint8_t> chip;
        opll_->saveState(chip);
        StateWriter w;
        uint64_t pos = opll_->clockPosition();
        w(pos, chip);
        block("OPLL", w.data());
    }
    if (quick) {
        // No thumbnail: nobody looks at a rewind snapshot.
    } else if (!loadedThumbnail_.empty()) {
        // Nothing has run since a state was loaded: the picture on screen is still the
        // one from before, so keep the loaded state's thumbnail.
        block("THMB", loadedThumbnail_);
    } else {
        // Thumbnail: the 2D picture at half size, without the widescreen columns.
        const VideoFrameMd& f = vdp_.frame();
        const int step = f.interlaced ? 2 : 1;
        uint16_t tw = static_cast<uint16_t>(f.consoleWidth() / 2), th = static_cast<uint16_t>(f.height / step / 2);
        StateWriter w;
        w(tw, th);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                uint32_t c = f.pixel(f.wideLeft + x * 2, y * 2 * step) & 0xFFFFFF;
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
    bool cacheFromBoot = false;  // the state comes from a version whose boot turns the 32X's cache on
    uint16_t overclock = 100;  // states without the block are from a console at its speed
    std::vector<uint8_t> thumbnail, addOnState;
    dmaBusUntil_ = 0;  // states without the block: no DMA holds the bus
    refreshClocks_ = 0;  // states without the block are from before the refresh
    refreshAfterFetch_ = false;
    m68kBusStall_ = 0;
    cpu_.dropPrefetch();
    z80BusGrantAt_ = 0;  // nor the bus grant delay
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
            cpu_.settleIrq();
            cpu_.forgetRoutine();
        } else if (id == fourcc("Z80 ")) z80_.state(b);
        else if (id == fourcc("VDP ")) vdp_.state(b);
        else if (id == fourcc("VDPF")) vdp_.fifoState(b);
        else if (id == fourcc("SATC")) vdp_.satState(b);
        else if (id == fourcc("PSG ")) psg_.state(b);
        else if (id == fourcc("BUS ")) busState(b);
        else if (id == fourcc("RFSH")) b(refreshClocks_);
        else if (id == fourcc("PFCH")) cpu_.prefetchState(b);
        else if (id == fourcc("ZBGA")) b(z80BusGrantAt_);
        else if (id == fourcc("IRQS")) {
            cpu_.irqState(b);
            if (mcd_) mcd_->sub().irqState(b);
        }
        else if (id == fourcc("IO  ")) io_.state(b);
        else if (id == fourcc("IOTH")) io_.riseState(b);
        else if (id == fourcc("DMAB")) b(dmaBusUntil_);
        else if (id == fourcc("BSTL")) b(m68kBusStall_, refreshAfterFetch_);
        else if (id == fourcc("IOSR")) io_.serialState(b);
        else if (id == fourcc("CART")) cart_.state(b);
        else if (id == fourcc("EEPR")) cart_.eepromState(b);
        else if (id == fourcc("THMB")) thumbnail = data;
        else if (id == fourcc("OVCK")) b(overclock);
        else if (id == fourcc("32X ")) {
            if (mars_) {
                mars_->state(b);
                mars_->clearTiming();  // a state without it (older) starts with empty caches
            }
        }
        else if (id == fourcc("32XI")) {
            if (mars_) mars_->ioState(b);
        }
        else if (id == fourcc("32XC") || id == fourcc("32X2")) {
            if (mars_) mars_->timingState(b);
            cacheFromBoot = id == fourcc("32X2");
        }
        else if (id == fourcc("PAPR")) {
            if (paprium_) paprium_->state(b);
        }
        else if (id == fourcc("SVP ")) {
            if (svp_) svp_->state(b);
        }
        else if (id == fourcc("MCD ")) {
            if (mcd_) {
                mcd_->state(b);
                mcd_->sub().settleIrq();
            }
        }
        else if (id == fourcc("SMS ")) {
            if (sms_) sms_->state(b);
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
        else if (id == fourcc("OPLL")) {
            uint64_t pos = 0;
            std::vector<uint8_t> chip;
            b(pos, chip);
            if (opll_ && b.ok() && opll_->loadState(chip)) opll_->setClockPosition(pos);
            else if (opll_) ok = false;
        }
        // Unknown blocks (from newer versions) are skipped.
        if (!b.ok()) ok = false;
    }
    if (ok && mars_ && !cacheFromBoot) mars_->bootCache();  // older states left it off
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
    setOverclock(overclock);
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
    if (opll_) {
        opll_->samples().clear();
        opll_->setVolume(sms_->fmAudible() ? 100 : 0);
    }
    psg_.clearSamples();
    if (mars_) mars_->samples().clear();
    if (mcd_) mcd_->samples().clear();
    if (paprium_) paprium_->samples().clear();
    frames_ = frames;
    vdpFreeAt_ = 0;
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
