#include "io.h"

namespace plumsg {

namespace {

// About 1.5 ms of master clocks: after that long without TH changing, a 6-button pad
// goes back to its first state.
constexpr uint64_t kSixButtonTimeout = 80000;
// About 7.5 us of master clocks: how long a TH pin let go while low still reads low.
// Trouble Shooter switches TH to an input and reads A and Start right after, 140 clocks
// later.
constexpr uint64_t kThRiseTime = 400;

}  // namespace

void Io::reset() {
    data_ = {0x7F, 0x7F, 0x7F};
    ctrl_ = {0, 0, 0};
    thCount_ = {0, 0};
    lastTh_ = {true, true};
    thTime_ = {0, 0};
    thRelease_ = {kNotRising, kNotRising};
    txData_ = {0xFF, 0xFF, 0xFF};
    serialCtrl_ = {0, 0, 0};
}

// TH reads as the value written when it is an output, else it floats high (after a
// moment if it was low).
bool Io::thLevel(int port, uint64_t now) const {
    if (ctrl_[port] & 0x40) return (data_[port] & 0x40) != 0;
    return thRelease_[port] == kNotRising || now < thRelease_[port] || now - thRelease_[port] >= kThRiseTime;
}

uint8_t Io::readPort(int port, uint64_t now) {
    // An empty port: its pins float high, and a pad's TH-low reading (left and right
    // low) never shows up.
    if (port == 2 || types_[port] == PadType::None)
        return static_cast<uint8_t>((data_[port] & ctrl_[port]) | (0x7F & ~ctrl_[port]));
    const PadState& p = pads_[port];
    const bool th = thLevel(port, now);
    if (types_[port] == PadType::SixButton && now - thTime_[port] > kSixButtonTimeout) thCount_[port] = 0;
    const int count = types_[port] == PadType::SixButton ? thCount_[port] : 0;

    // Buttons are active low.
    uint8_t v;
    if (th) {
        if (count == 3) {
            v = static_cast<uint8_t>((p.z ? 0 : 1) | (p.y ? 0 : 2) | (p.x ? 0 : 4) | (p.mode ? 0 : 8) |
                                     (p.b ? 0 : 0x10) | (p.c ? 0 : 0x20));
        } else {
            v = static_cast<uint8_t>((p.up ? 0 : 1) | (p.down ? 0 : 2) | (p.left ? 0 : 4) | (p.right ? 0 : 8) |
                                     (p.b ? 0 : 0x10) | (p.c ? 0 : 0x20));
        }
        v |= 0x40;
    } else {
        if (count == 3) v = static_cast<uint8_t>((p.a ? 0 : 0x10) | (p.start ? 0 : 0x20));  // low nibble all 0
        else if (count == 4) v = static_cast<uint8_t>(0x0F | (p.a ? 0 : 0x10) | (p.start ? 0 : 0x20));
        else v = static_cast<uint8_t>((p.up ? 0 : 1) | (p.down ? 0 : 2) | (p.a ? 0 : 0x10) | (p.start ? 0 : 0x20));
    }
    // Pins set as outputs read back what was written.
    return static_cast<uint8_t>((data_[port] & ctrl_[port] & 0x7F) | (v & ~ctrl_[port] & 0x7F) | (data_[port] & 0x80));
}

uint8_t Io::read(uint32_t addr, uint64_t now) {
    switch ((addr >> 1) & 0x0F) {
        case 0: return version_;
        case 1: return readPort(0, now);
        case 2: return readPort(1, now);
        case 3: return readPort(2, now);
        case 4: return ctrl_[0];
        case 5: return ctrl_[1];
        case 6: return ctrl_[2];
        // Serial, three registers a port: TxData, RxData (nothing received), S-Ctrl.
        // Sega's softcheck writes and reads them back in its I/O port test.
        case 7: case 10: case 13: return txData_[(((addr >> 1) & 0x0F) - 7) / 3];
        case 9: case 12: case 15: return serialCtrl_[(((addr >> 1) & 0x0F) - 9) / 3];
        default: return 0x00;
    }
}

void Io::write(uint32_t addr, uint8_t value, uint64_t now) {
    const int reg = (addr >> 1) & 0x0F;
    if (reg == 7 || reg == 10 || reg == 13) txData_[(reg - 7) / 3] = value;
    else if (reg == 9 || reg == 12 || reg == 15) serialCtrl_[(reg - 9) / 3] = value & 0xF8;
    if (reg >= 1 && reg <= 3) data_[reg - 1] = value;
    else if (reg >= 4 && reg <= 6) {
        const int port = reg - 4;
        if (port < 2 && (ctrl_[port] & 0x40) && !(value & 0x40))
            thRelease_[port] = (data_[port] & 0x40) ? kNotRising : now;
        ctrl_[port] = value;
    } else return;
    for (int port = 0; port < 2; ++port) {
        const bool th = thLevel(port, now);
        if (th == lastTh_[port]) continue;
        if (now - thTime_[port] > kSixButtonTimeout) thCount_[port] = 0;
        // The counter steps on each falling edge: after the third, TH low reads 0000 in
        // the direction bits and TH high X/Y/Z/Mode; after the fourth, TH low reads 1111.
        if (!th) thCount_[port] = static_cast<uint8_t>(thCount_[port] >= 4 ? 1 : thCount_[port] + 1);
        lastTh_[port] = th;
        thTime_[port] = now;
    }
}

}  // namespace plumsg
