#pragma once

#include <array>
#include <cstdint>

#include "cartridge.h"
#include "io.h"
#include "vdp_sms.h"

namespace plumsg {

// The Master System side of the console: its VDP, the memory map with the Sega mapper,
// and its I/O ports. The Mega Drive runs Master System cartridges in this mode itself
// (with the Power Base Converter), and plumsg does the same: MegaDrive keeps the Z80,
// the PSG, the scheduler and the savestates, and hands the Z80's memory and port
// accesses here. The 8 KB of RAM are the Z80's own RAM of the Mega Drive; the
// cartridge's battery RAM is the cartridge's save RAM.
//
// Sega mapper: 0x0000-0x03FF is always the first 1 KB of the ROM; slots of 16 KB at
// 0x0400 (rest of slot 0), 0x4000 and 0x8000 are picked by writes to 0xFFFD, 0xFFFE and
// 0xFFFF, and 0xFFFC bit 3 puts one of two 16 KB pages of cartridge RAM (bit 2) in the
// third slot instead. RAM at 0xC000-0xDFFF, repeated at 0xE000-0xFFFF.
//
// Codemasters mapper (told by the checksum in its own header at 0x7FE0): the three
// slots are picked by writes to 0x0000, 0x4000 and 0x8000, the first one too, and bit 7
// of the 0x4000 write puts 8 KB of cartridge RAM at 0xA000-0xBFFF (Ernie Els Golf).
//
// The Game Gear is the same machine in a handheld: with `gameGear` its VDP has the
// Game Gear's colours and screen, port 0x00 has the START button and the region, ports
// 0x01-0x05 the link cable (nothing plugged in), and there is one pad and no pause
// button. The PSG's stereo (port 0x06) the console handles, as it has the PSG.
class MasterSystem {
public:
    MasterSystem(Cartridge& cart, std::array<uint8_t, 0x2000>& ram, VideoFrameMd& frame, bool gameGear = false);
    bool gameGear() const { return gameGear_; }

    void powerOn();  // clears the VDP and the mapper
    void reset();

    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t value);
    // Ports, but for the PSG (0x40-0x7F written), which the console writes itself.
    // `cycle` is how far into the line the Z80 is, for the H counter.
    uint8_t in(uint8_t port, int cycle);
    void out(uint8_t port, uint8_t value);

    // Mega Drive pads: B and A are button 1, C is button 2. Start is the pause button (on
    // the Game Gear, its START),
    // which makes an NMI when pressed: pausePressed(), called once a frame, says whether
    // it went down since the last frame.
    void setPads(const PadState& p1, const PadState& p2) {
        pads_[0] = p1;
        pads_[1] = p2;
    }
    bool pausePressed() {
        if (gameGear_) return false;
        const bool pressed = pads_[0].start && !pause_;
        pause_ = pads_[0].start;
        return pressed;
    }

    // Japanese consoles read the TH lines back differently from export ones; some
    // games tell where they are from that.
    void setJapanese(bool on) { japanese_ = on; }
    // The FM sound unit (YM2413, built into the Japanese Master System): its ports
    // 0xF0-0xF2 answer only when it is there. Port 0xF2 is the audio control: bit 0
    // turns the FM on; the PSG plays unless only the FM is on.
    void setFmPresent(bool on) { fmPresent_ = on; }
    bool fmPresent() const { return fmPresent_; }
    uint8_t fmControl() const { return fmControl_; }
    void setFmControl(uint8_t value) { fmControl_ = value & 0x03; }
    bool fmAudible() const { return fmPresent_ && (fmControl_ & 1); }
    bool psgAudible() const { return !fmPresent_ || fmControl_ != 1; }

    VdpSms& vdp() { return vdp_; }
    const VdpSms& vdp() const { return vdp_; }
    // Mapper registers: as written to 0xFFFC-0xFFFF, or with the Codemasters mapper the
    // banks of the three slots in [1..3] and its RAM switch in [0].
    const std::array<uint8_t, 4>& mapper() const { return mapper_; }
    bool codemasters() const { return codemasters_; }

    template <class Ar>
    void state(Ar& ar) {
        ar(mapper_, ioControl_, memoryControl_, pause_);
        vdp_.state(ar);
        // Added after the first version: states without it load with the FM unit quiet.
        if constexpr (Ar::kLoading) {
            fmControl_ = 0;
            if (ar.atEnd()) return;
        }
        ar(fmControl_);
        if constexpr (Ar::kLoading) {
            if (ar.atEnd()) return;
        }
        vdp_.gameGearState(ar);
    }

private:
    uint8_t romByte(int bank, uint16_t offset) const;
    uint8_t portA() const;
    uint8_t portB() const;

    Cartridge& cart_;
    std::array<uint8_t, 0x2000>& ram_;
    VdpSms vdp_;
    std::array<uint8_t, 4> mapper_{0, 0, 1, 2};
    bool codemasters_ = false;
    bool gameGear_ = false;
    uint8_t ioControl_ = 0xFF;    // port 0x3F
    uint8_t memoryControl_ = 0;   // port 0x3E
    std::array<PadState, 2> pads_{};
    bool pause_ = false;          // Start held at the last frame
    bool japanese_ = false;
    bool fmPresent_ = false;
    uint8_t fmControl_ = 0;
};

}  // namespace plumsg
