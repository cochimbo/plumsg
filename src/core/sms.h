#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
//
// 93C46 EEPROM (World Series Baseball, World Series Baseball '95, Majors Pro Baseball,
// told by their CRC): these Game Gear cartridges save to a serial EEPROM of 64 words.
// 0xFFFC bit 3 puts its lines at 0x8000 instead of the cartridge RAM (bit 7 resets
// it): written, bit 0 is the data in, bit 1 the clock and bit 2 the chip select; read,
// bit 0 is the data out. Its 128 bytes are the first ones of the cartridge's save RAM.
// Codemasters cartridges have a header of their own at 0x7FE0 whose checksum word is
// followed by its complement.
bool isCodemasters(const std::vector<uint8_t>& rom);
// The Game Gear cartridges with the 93C46 EEPROM, by the CRC of their ROM.
bool hasGameGearEeprom(uint32_t romCrc);
// A dump of the console's own BIOS, told by its file name ("[BIOS] Alex Kidd in Miracle
// World (USA, Europe, Brazil) (En).sms", "bios13.sms").
bool isMasterSystemBiosFile(const std::string& path);
// The controller a Master System game needs in its first port, by the CRC of its ROM:
// LightPhaser for the light gun games (Safari Hunt, Shooting Gallery, Operation Wolf...),
// Paddle for the paddle ones (Woody Pop, Alex Kidd BMX Trial...), else SixButton.
PadType masterSystemControllerFor(uint32_t romCrc);

class MasterSystem {
public:
    MasterSystem(Cartridge& cart, std::array<uint8_t, 0x2000>& ram, VideoFrameMd& frame, bool gameGear = false);
    bool gameGear() const { return gameGear_; }

    void powerOn();  // clears the VDP and the mapper
    // The ROM is the console's BIOS, not a cartridge: port 0x3E switches it off (bit 3),
    // and with it off nothing answers in the slots, as no cartridge, card or expansion is
    // plugged in. A BIOS looks in the slots for a game that way, and without it finds
    // itself there and starts itself again and again (issue #136); with it, it finds
    // nothing and runs the game it has inside, or tells to put one in.
    void setBios(bool on) { bios_ = on; }
    bool bios() const { return bios_; }
    void reset();

    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t value);
    // Ports, but for the PSG (0x40-0x7F written), which the console writes itself.
    // `cycle` is how far into the line the Z80 is, for the H counter.
    uint8_t in(uint8_t port, int cycle);
    void out(uint8_t port, uint8_t value, int cycle = 0);

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
        const bool held = pads_[0].start || (controller(0) == PadType::LightPhaser && guns_[0].start);
        const bool pressed = held && !pause_;
        pause_ = held;
        return pressed;
    }

    // What is plugged in each port, as chosen (MegaDrive passes its own). A pad, or the
    // Mega Drive's light guns, which the Master System reads as a pad. The Light Phaser
    // and the paddle answer in their own way, the first port's on its own too when the
    // game needs one (masterSystemControllerFor) and a pad was chosen there, unless
    // setAutoControllers(false) (netplay, which does not carry where they point).
    void setPadType(int port, PadType t) { types_[static_cast<size_t>(port & 1)] = t; }
    void setAutoControllers(bool on) { autoControllers_ = on; }
    // What the game sees in a port; nothing but pads on the Game Gear.
    PadType controller(int port) const;
    // Where each port's Light Phaser points (pixels of the picture, as the Mega Drive's
    // guns) and its trigger, and each port's paddle.
    void setGun(int port, const GunState& s) { guns_[static_cast<size_t>(port & 1)] = s; }
    void setPaddle(int port, const PaddleState& s) { paddles_[static_cast<size_t>(port & 1)] = s; }

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
    bool eeprom() const { return eepromPresent_; }

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
        if constexpr (Ar::kLoading) {
            if (ar.atEnd()) return;
        }
        vdp_.timingState(ar);
        // Added with the Game Gear's link port and EEPROM.
        if constexpr (Ar::kLoading) {
            link_ = kLinkReset;
            eeprom_ = Eeprom{};
            if (ar.atEnd()) return;
        }
        ar(link_, eeprom_.enabled, eeprom_.cs, eeprom_.clk, eeprom_.data, eeprom_.writeEnable, eeprom_.state,
           eeprom_.opcode, eeprom_.cycles, eeprom_.buffer);
        // Added with the Light Phaser and the paddle.
        if constexpr (Ar::kLoading) {
            gunLit_ = {};
            paddleHigh_ = {};
            paddleReads_ = {};
            if (ar.atEnd()) return;
        }
        ar(gunLit_, paddleHigh_, paddleReads_);
    }

private:
    // The Game Gear's link port registers, ports 0x01-0x05 (index 0 unused): parallel
    // data, its direction (1 = input) with the NMI enable in bit 7, the serial data out
    // and in, and the serial control. Nothing is plugged in: the inputs read high and
    // no byte ever comes.
    static constexpr std::array<uint8_t, 6> kLinkReset{0xFF, 0x7F, 0xFF, 0x00, 0xFF, 0x00};
    struct Eeprom {
        enum State : uint8_t { WaitStart, Opcode, Write, Read, Standby };
        bool enabled = false;  // 0xFFFC bit 3
        bool cs = false, clk = false, data = true, writeEnable = false;
        uint8_t state = WaitStart;
        uint8_t opcode = 0, cycles = 0;
        uint16_t buffer = 0;
    };
    void eepromWrite(uint8_t value);
    uint8_t eepromRead() const;
    uint16_t eepromWord(int index) const;
    void setEepromWord(int index, uint16_t value);

    uint8_t romByte(int bank, uint16_t offset) const;
    uint8_t portA(int cycle);
    uint8_t portB(int cycle);
    // A port's pins, active low: up, down, left, right, TL, TR in bits 0-5 and TH in 6.
    uint8_t portPins(int port, int cycle, bool thRead);
    bool thPin(int port) const;
    bool phaserSeesLight(int port, int cycle) const;
    uint8_t thLevels() const;

    Cartridge& cart_;
    std::array<uint8_t, 0x2000>& ram_;
    VdpSms vdp_;
    std::array<uint8_t, 4> mapper_{0, 0, 1, 2};
    bool codemasters_ = false;
    bool eepromPresent_ = false;
    Eeprom eeprom_;
    std::array<uint8_t, 6> link_ = kLinkReset;
    bool bios_ = false;
    bool gameGear_ = false;
    uint8_t ioControl_ = 0xFF;    // port 0x3F
    uint8_t memoryControl_ = 0;   // port 0x3E
    std::array<PadState, 2> pads_{};
    std::array<PadType, 2> types_{PadType::SixButton, PadType::SixButton};
    bool autoControllers_ = true;
    PadType autoController_ = PadType::SixButton;  // what the game needs in the first port
    // The Light Phaser: where it is aimed, whether it saw the beam at the last read of
    // its TH (a new sight latches the H counter), and what the H counter says for its x
    // (x / 2 plus this, which the games expect).
    std::array<GunState, 2> guns_{};
    std::array<bool, 2> gunLit_{};
    int phaserOffset_ = 20;
    // The paddle: its knob and button; and, as the Japanese paddle switches on its own
    // between the low and the high four bits of the knob, which it shows now (TR high
    // with the high ones) and the reads since it switched.
    std::array<PaddleState, 2> paddles_{};
    std::array<bool, 2> paddleHigh_{};
    std::array<uint8_t, 2> paddleReads_{};
    bool pause_ = false;          // Start held at the last frame
    bool japanese_ = false;
    bool fmPresent_ = false;
    uint8_t fmControl_ = 0;
};

}  // namespace plumsg
