// Minimal CP/M environment to run Frank Cringle's ZEXDOC / ZEXALL Z80 exercisers.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/z80.h"

namespace {

class FlatMemory : public plumsg::Z80Bus {
public:
    uint8_t mem[0x10000] = {};
    uint8_t read(uint16_t a) override { return mem[a]; }
    void write(uint16_t a, uint8_t v) override { mem[a] = v; }
    uint8_t in(uint16_t) override { return 0xFF; }
    void out(uint16_t, uint8_t) override {}
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: zex_runner zexdoc.com\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> program((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (program.empty()) {
        std::fprintf(stderr, "no se puede leer %s\n", argv[1]);
        return 2;
    }

    FlatMemory bus;
    std::copy(program.begin(), program.end(), bus.mem + 0x100);
    bus.mem[0x0005] = 0xC9;  // BDOS entry: RET, the call itself is trapped below
    bus.mem[0x0006] = 0x00;  // top of TPA, used by the exercisers to set SP
    bus.mem[0x0007] = 0xF0;
    plumsg::Z80 cpu(bus);
    cpu.pc = 0x100;
    cpu.sp = 0xF000;

    bool failed = false;
    uint64_t cycles = 0;
    while (cpu.pc != 0) {
        if (cpu.pc == 0x0005) {
            if (cpu.bc.b.l == 2) {
                std::putchar(cpu.de.b.l);
            } else if (cpu.bc.b.l == 9) {
                for (uint16_t a = cpu.de.w; bus.mem[a] != '$'; ++a) {
                    // "ERROR" in the output means a test group failed.
                    if (bus.mem[a] == 'E' && bus.mem[a + 1] == 'R' && bus.mem[a + 2] == 'R') failed = true;
                    std::putchar(bus.mem[a]);
                }
            }
            std::fflush(stdout);
        }
        cycles += static_cast<uint64_t>(cpu.step());
    }
    std::printf("\n%llu ciclos\n", static_cast<unsigned long long>(cycles));
    return failed ? 1 : 0;
}
