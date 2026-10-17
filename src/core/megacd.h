#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "cd_image.h"
#include "m68k.h"

namespace plumsg {

// The Mega CD: a second 68000 (the sub CPU, at 12.5 MHz) with 512 KB of program RAM,
// 256 KB of word RAM it shares with the console's 68000, 8 KB of backup RAM, the CD
// drive (its controller, the CDC, and the drive itself, the CDD), the graphics chip
// that rotates and scales stamps, the RF5C164 PCM sound chip, CD audio and the gate
// array registers that tie it all together. The console owns it and calls it for
// every 68000 access in the areas the Mega CD answers (the BIOS and the windows onto
// its memory, which take the cartridge's place, and 0xA12000-0xA1203F), runs the sub
// CPU alongside its own and mixes its sound.
//
// It needs the real BIOS (one per region): the sub CPU runs the BIOS's code, which it
// is handed by the console's 68000 at boot. The drive is emulated at the level of its
// commands, as other emulators do.
class MegaCd : private M68kBus {
public:
    static constexpr double kSubClock = 12500000.0;

    MegaCd();

    void setBios(std::vector<uint8_t> bios) { bios_ = std::move(bios); }
    bool hasBios() const { return bios_.size() >= 0x20000; }
    // The disc in the tray (null for none); it is not owned.
    void insert(CdImage* disc);

    void powerOn();  // clears memory (but not the backup RAM)
    void reset();    // the console's reset button
    // Runs the sub CPU up to `masterClock` of the console.
    void runUntil(uint64_t masterClock);
    void rebaseClock() { subClock_ = 0; }
    void setMasterClock(double hz) { master_ = hz; }

    // The console's 68000 side. `read68k` returns false when the address is not the
    // Mega CD's. `upper` and `lower` say which bytes are accessed.
    bool read68k(uint32_t addr, uint16_t& value);
    bool write68k(uint32_t addr, uint16_t value, bool upper, bool lower);
    // A VDP DMA reading word RAM gets each word one read late, as on the real thing
    // (games correct the first word themselves).
    bool dmaRead(uint32_t addr, uint16_t& value);

    // Stereo PCM and CD audio at the output rate produced since the last call.
    void setSampleRate(double hz);
    std::vector<int16_t>& samples();

    // Backup RAM, saved by the frontend. format() writes an empty, formatted one.
    std::array<uint8_t, 0x2000>& backupRam() { return bram_; }
    void formatBackupRam();
    bool backupRamChanged() const { return bramChanged_; }
    void clearBackupRamChanged() { bramChanged_ = false; }

    M68000& sub() { return sub_; }
    const std::array<uint16_t, 8>& command() const { return comm_.command; }
    const std::array<uint16_t, 8>& status() const { return comm_.status; }
    std::array<uint8_t, 0x80000>& programRam() { return prg_; }
    std::array<uint16_t, 0x20000>& wordRam() { return wram_; }
    bool subRunning() const { return !io_.halt; }

    template <class Ar>
    void state(Ar& ar) {
        sub_.state(ar);
        ar(prg_, wram_, bram_, subClock_, resetPending_, counter_.divider, counter_.dma, counter_.cdda);
        ar(io_.run, io_.request, io_.halt, io_.wramLatch, io_.wramMode, io_.wramSwitchRequest, io_.wramSwitch,
           io_.wramSelect, io_.wramPriority, io_.prgBank, io_.prgProtect, io_.vectorLevel4, led_);
        irqState(ar, gpu_.irq);
        irqState(ar, external_);
        irqState(ar, timer_.irq);
        irqState(ar, cdd_.irq);
        irqState(ar, cdc_.irq);
        irqState(ar, subcodeIrq_);
        ar(comm_.cfm, comm_.cfs, comm_.command, comm_.status);
        ar(cdc_.ram, cdc_.address, cdc_.stopwatch);
        irqState(ar, cdc_.decoderIrq);
        irqState(ar, cdc_.transferIrq);
        irqState(ar, cdc_.commandIrq);
        ar(cdc_.commandFifo, cdc_.commandRead, cdc_.commandWrite, cdc_.commandEmpty);
        ar(cdc_.statusFifo, cdc_.statusRead, cdc_.statusWrite, cdc_.statusEmpty, cdc_.statusEnable, cdc_.statusActive,
           cdc_.statusBusy, cdc_.statusWait);
        auto& t = cdc_.transfer;
        ar(t.destination, t.address, t.source, t.target, t.pointer, t.length, t.enable, t.active, t.busy, t.wait,
           t.ready, t.completed);
        ar(cdc_.decoderEnable, cdc_.decoderMode, cdc_.decoderForm, cdc_.decoderValid, cdc_.header, cdc_.subheader,
           cdc_.control);
        ar(cdd_.counter, cdd_.status, cdd_.seeking, cdd_.latency, cdd_.sector, cdd_.sample, cdd_.track, cdd_.tocRead,
           cdd_.subcodePosition, cdd_.hostClockEnable, cdd_.statusPending, cdd_.statusRegs, cdd_.commandRegs,
           cdd_.subcode);
        ar(cdd_.dacRate, cdd_.dacDeemphasis, cdd_.attenuator, cdd_.attenuated);
        ar(timer_.frequency, timer_.counter);
        ar(gpu_.fontBackground, gpu_.fontForeground, gpu_.fontData, gpu_.repeat, gpu_.tileSize, gpu_.mapSize,
           gpu_.mapBase, gpu_.mapAddress, gpu_.imageBase, gpu_.imageOffset, gpu_.imageVcells, gpu_.imageVdots,
           gpu_.imageHdots, gpu_.imageAddress, gpu_.vectorBase, gpu_.vectorAddress, gpu_.active, gpu_.counter,
           gpu_.period);
        ar(pcm_.ram, pcm_.enable, pcm_.bank, pcm_.channel);
        for (auto& c : pcm_.channels) ar(c.enable, c.envelope, c.pan, c.step, c.loop, c.start, c.address);
        ar(out_.pcm.next, out_.pcm.prevL, out_.pcm.prevR, out_.cdda.next, out_.cdda.prevL, out_.cdda.prevR);
        ar(out_.pcmOut, out_.cddaOut);
        out_.mixed.clear();
        cachedLba_ = kNoSector;
        updateIrq();
    }

private:
    struct Irq {
        bool enable = false, pending = false;
        bool raise() {
            if (pending) return false;
            pending = enable;
            return true;
        }
        bool lower() {
            if (!pending) return false;
            pending = false;
            return true;
        }
    };
    template <class Ar>
    static void irqState(Ar& ar, Irq& i) {
        ar(i.enable, i.pending);
    }

    // Sub CPU bus.
    uint8_t read8(uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;
    void write16(uint32_t addr, uint16_t value) override;
    void irqAcknowledge(int level) override;
    uint16_t subRead(uint32_t addr, bool upper, bool lower);
    void subWrite(uint32_t addr, uint16_t value, bool upper, bool lower);
    uint16_t readIo(uint32_t addr, bool upper, bool lower);
    void writeIo(uint32_t addr, uint16_t value, bool upper, bool lower);
    // Console side.
    uint16_t readExternal(uint32_t addr);
    void writeExternal(uint32_t addr, uint16_t value, bool upper, bool lower);
    uint16_t readExternalIo(uint32_t addr);
    void writeExternalIo(uint32_t addr, uint16_t value, bool upper, bool lower);
    uint32_t wordRamIndex(uint32_t addr) const;  // console address to word RAM word

    void step(int clocks);  // runs everything but the CPU for `clocks` sub CPU cycles
    void updateIrq();
    void resetCpu();
    void resetPeripheral();

    // CDC (Sanyo LC8951).
    void cdcPoll();
    void cdcDecode(int32_t sector);
    uint8_t cdcRead();
    void cdcWrite(uint8_t data);
    void cdcDma();
    uint16_t cdcTransferRead();
    void cdcStart();
    void cdcComplete();
    void cdcStop();
    void cdcPower();
    // CDD (the drive's microcontroller).
    void cddClock();
    void cddAdvance();
    void cddSample();
    void cddProcess();
    void cddChecksum();
    void cddInsert();
    void cddPower();
    void cddReadSubcode();
    int trackAt(int32_t lba) const;  // track number, 0 if none
    bool trackAudio(int track) const;
    bool trackData(int track) const;
    double cddPosition(int32_t sector) const;
    void cachedSector(int32_t lba);
    // GPU.
    void gpuStep(int clocks);
    void gpuRender(uint32_t address, int width);
    void gpuStart();
    uint8_t gpuRead(uint32_t nibble) const;
    void gpuWrite(uint32_t nibble, uint8_t data);
    // PCM.
    void pcmClock();
    uint8_t pcmRead(uint32_t addr);
    void pcmWrite(uint32_t addr, uint8_t data);

    M68000 sub_{*this};
    std::vector<uint8_t> bios_;
    CdImage* disc_ = nullptr;
    std::array<uint8_t, 0x80000> prg_{};
    std::array<uint16_t, 0x20000> wram_{};  // 1 Mbit mode: the two banks alternate word by word
    std::array<uint8_t, 0x2000> bram_{};
    bool bramChanged_ = false;

    double master_ = 53693175.0;
    uint64_t subClock_ = 0;  // sub CPU cycles run since the last rebase
    bool resetPending_ = true;  // the sub CPU fetches its vectors when it next runs

    struct Counter {
        uint32_t divider = 0, dma = 0;
        double cdda = 0;
    } counter_;

    struct Io {
        bool run = false, request = false, halt = true;
        uint16_t wramLatch = 0;
        bool wramMode = false;  // false: 2 Mbit, true: 1 Mbit
        bool wramSwitchRequest = false, wramSwitch = false, wramSelect = false;
        uint8_t wramPriority = 0;
        uint8_t prgBank = 0, prgProtect = 0;
        uint32_t vectorLevel4 = 0xFFFFFFFF;
    } io_;
    uint8_t led_ = 0;
    Irq external_, subcodeIrq_;

    struct Comm {
        uint8_t cfm = 0, cfs = 0;
        std::array<uint16_t, 8> command{}, status{};
    } comm_;

    struct Transfer {
        uint8_t destination = 0;
        uint32_t address = 0;  // 19 bits
        uint16_t source = 0, target = 0, pointer = 0, length = 0;
        bool enable = false, active = false, busy = false, wait = false, ready = false, completed = false;
    };
    struct Cdc {
        std::array<uint16_t, 0x2000> ram{};
        uint8_t address = 0;
        uint16_t stopwatch = 0;
        Irq irq, decoderIrq, transferIrq, commandIrq;
        std::array<uint8_t, 8> commandFifo{};
        uint8_t commandRead = 0, commandWrite = 0;
        bool commandEmpty = true;
        std::array<uint8_t, 8> statusFifo{};
        uint8_t statusRead = 0, statusWrite = 0;
        bool statusEmpty = true, statusEnable = false, statusActive = false, statusBusy = false, statusWait = false;
        Transfer transfer;
        bool decoderEnable = false, decoderMode = false, decoderForm = false, decoderValid = false;
        std::array<uint8_t, 4> header{}, subheader{};
        uint32_t control = 0;  // the CTRL0-2 bits, see kCtrl*
    } cdc_;

    struct Cdd {
        Irq irq;
        uint16_t counter = 0;
        uint8_t status = 0xB;  // no disc
        uint8_t seeking = 0;
        uint16_t latency = 0;
        int32_t sector = 0;
        uint16_t sample = 0;
        int track = 0;
        bool tocRead = false;
        uint8_t subcodePosition = 0;
        bool hostClockEnable = false, statusPending = false;
        std::array<uint8_t, 10> statusRegs{}, commandRegs{};
        std::array<uint16_t, 64> subcode{};
        bool dacRate = false;
        uint8_t dacDeemphasis = 0;
        uint16_t attenuator = 0x4000, attenuated = 0x4000;
    } cdd_;

    struct Timer {
        Irq irq;
        uint8_t frequency = 0, counter = 0;
    } timer_;

    struct Gpu {
        Irq irq;
        uint8_t fontBackground = 0, fontForeground = 0;
        uint16_t fontData = 0;
        bool repeat = false, tileSize = false, mapSize = false;
        uint32_t mapBase = 0, mapAddress = 0;
        uint32_t imageBase = 0;
        uint8_t imageOffset = 0, imageVcells = 0, imageVdots = 0;
        uint16_t imageHdots = 0;
        uint32_t imageAddress = 0;
        uint32_t vectorBase = 0, vectorAddress = 0;
        bool active = false;
        uint32_t counter = 0, period = 0;
    } gpu_;

    struct PcmChannel {
        bool enable = false;
        uint8_t envelope = 0, pan = 0xFF;
        uint16_t step = 0, loop = 0;
        uint8_t start = 0;
        uint32_t address = 0;  // 27 bits: 16.11 fixed point
    };
    struct Pcm {
        std::array<uint8_t, 0x10000> ram{};
        bool enable = false;
        uint8_t bank = 0, channel = 0;
        std::array<PcmChannel, 8> channels{};
    } pcm_;

    // Linear interpolation of the PCM (32.6 kHz) and CD audio (44.1 kHz) to the
    // frontend's rate.
    struct Upsampler {
        double ratio = 1;  // input samples per output sample
        double next = 0;   // where the next output sample falls after prev
        int prevL = 0, prevR = 0;
        void push(int l, int r, std::vector<int16_t>& out);
    };
    struct Output {
        Upsampler pcm, cdda;
        std::vector<int16_t> pcmOut, cddaOut, mixed;
    } out_;

    static constexpr int32_t kNoSector = INT32_MIN;
    int32_t cachedLba_ = kNoSector;
    std::array<uint8_t, CdImage::kSectorSize> sector_{};
};

}  // namespace plumsg
