// Core tests that need no external files: the cartridge (header, SMD, zip, SRAM), the
// pads, the PSG, the VDP ports, the built-in test ROM drawn and heard, savestates
// (determinism, round trip, damaged files), rewinding and the speed controls, the
// pseudo-3D planes, the games' 3D profiles, the cheat codes, the library's names and
// netplay over a simulated network, and RetroAchievements against a made-up server.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <filesystem>

#include "megadrive.h"
#include "ym2413.h"
#include "sh2.h"
#include "system.h"
#include "miniz.h"
#include "render/planes.h"
#include "render/profile3d.h"
#include "rewind.h"
#include "test_rom.h"
#include "frontend/achievements.h"
#include "frontend/game_info_data.h"
#include "frontend/library_names.h"
#include "mars/mars_program.h"
#include "megacd/megacd_program.h"
#include "cd_image.h"
#include "netplay.h"
#include "notation.h"
#include "pad_test_rom.h"
#include "sms_test_rom.h"
#include "music_rom.h"
#include "song_extract.h"
#include "timbre.h"

using namespace plumsg;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

uint32_t frameHash(const VideoFrameMd& f) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            h ^= f.pixel(x, y);
            h *= 16777619u;
        }
    return h;
}

uint32_t soundHash(const std::vector<int16_t>& s) {
    uint32_t h = 2166136261u;
    for (int16_t v : s) {
        h ^= static_cast<uint16_t>(v);
        h *= 16777619u;
    }
    return h;
}

void testHeader() {
    Cartridge cart;
    cart.loadData(testrom::build());
    check(cart.header().system.rfind("SEGA", 0) == 0, "header: system name");
    check(cart.header().title == "PLUMSG TEST ROM", "header: overseas title");
    check(cart.header().japan && cart.header().usa && cart.header().europe, "header: regions JUE");
    check(cart.preferredRegion() == Region::UsaNtsc, "header: USA preferred when every region is listed");
    check(cart.read16(4) == 0x0000 && cart.read16(6) == 0x0200, "reset vector read big-endian");

    // Another World (Europe) spells its region out.
    std::vector<uint8_t> rom = testrom::build();
    std::memcpy(&rom[0x1F0], "EUROPE          ", 16);
    cart.loadData(rom);
    check(cart.header().europe && !cart.header().usa && !cart.header().japan &&
              cart.preferredRegion() == Region::EuropePal,
          "header: \"EUROPE\" is Europe only");
}

void testSmd() {
    // An SMD dump: 512-byte copier header, then 16 KB blocks with the odd bytes in the
    // first half and the even bytes in the second.
    std::vector<uint8_t> bin = testrom::build();
    bin.resize(0x8000);
    std::vector<uint8_t> smd(512, 0);
    smd[0] = 2;     // number of 16 KB blocks
    smd[8] = 0xAA;  // SMD signature
    smd[9] = 0xBB;
    for (size_t block = 0; block < bin.size(); block += 0x4000) {
        std::vector<uint8_t> b(0x4000);
        for (size_t i = 0; i < 0x2000; ++i) {
            b[i] = bin[block + i * 2 + 1];
            b[0x2000 + i] = bin[block + i * 2];
        }
        smd.insert(smd.end(), b.begin(), b.end());
    }
    const std::string path = "core_tests_rom.smd";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(smd.data()), static_cast<std::streamsize>(smd.size()));
    Cartridge cart;
    std::string error;
    check(cart.loadFile(path, &error) && cart.rom() == bin, "SMD dumps are de-interleaved");
    std::remove(path.c_str());
}

bool writeZip(const std::string& path, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
    for (const auto& [name, data] : entries) {
        mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(),
                              static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION));
    }
    void* buf = nullptr;
    size_t size = 0;
    const bool ok = mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    if (ok) std::ofstream(path, std::ios::binary).write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return ok;
}

void testZipRom() {
    const std::vector<uint8_t> rom = testrom::build();
    const std::string path = "core_tests_rom.zip";
    writeZip(path, {{"LEEME.txt", {'h', 'o', 'l', 'a'}}, {"juego/Prueba.md", rom}, {"otro.gen", {0x00}}});
    Cartridge cart;
    std::string error;
    check(cart.loadFile(path, &error) && cart.rom() == rom, "ROM loads from a zip");
    writeZip(path, {{"LEEME.txt", {'h', 'o', 'l', 'a'}}});
    check(!cart.loadFile(path, &error) && !error.empty(), "zip without a ROM is rejected");
    std::remove(path.c_str());
}

// A folder named like a game (dropped on the window, or given on the command line) is
// refused with a message; reading it as a file used to throw and close plumsg.
void testFolderAsRom() {
    std::error_code ec;
    for (const char* name : {"core_tests_folder.md", "core_tests_folder.zip"}) {
        std::filesystem::create_directory(name, ec);
        Cartridge cart;
        std::string error;
        check(!cart.loadFile(name, &error) && !error.empty(), "a folder is not loaded as a ROM");
        detectSystem(name);  // the library asks every entry: must not throw either
        std::filesystem::remove(name, ec);
    }
}

void testSram() {
    // A 1 MB ROM with no SRAM in its header still gets the usual 64 KB at 0x200000.
    std::vector<uint8_t> rom = testrom::build();
    rom.resize(0x100000, 0xFF);
    Cartridge cart;
    cart.loadData(rom);
    cart.write8(0x200001, 0x5A);
    check(cart.read8(0x200001) == 0x5A && cart.hasSaveData(), "SRAM at 0x200000 for small ROMs");
}

// Talks to a cartridge's serial EEPROM as the games do: bit by bit on its SCL and SDA lines.
struct I2cMaster {
    Cartridge& cart;
    uint32_t sclAddr, sdaAddr;
    int sclBit, sdaBit;
    bool scl = true, sda = true;
    void set(bool c, bool d) {
        scl = c;
        sda = d;
        if (sclAddr == sdaAddr) {
            cart.write8(sclAddr, static_cast<uint8_t>((c ? 1 << sclBit : 0) | (d ? 1 << sdaBit : 0)));
        } else {
            cart.write8(sclAddr, static_cast<uint8_t>(c ? 1 << sclBit : 0));
            cart.write8(sdaAddr, static_cast<uint8_t>(d ? 1 << sdaBit : 0));
        }
    }
    bool line() const { return (cart.read8(sdaAddr) >> sdaBit) & 1; }
    void start() { set(true, true); set(true, false); set(false, false); }
    void stop() { set(false, false); set(true, false); set(true, true); }
    bool sendByte(uint8_t b) {
        for (int i = 7; i >= 0; --i) {
            set(false, (b >> i) & 1);
            set(true, (b >> i) & 1);
        }
        set(false, true);
        set(true, true);
        const bool ack = !line();
        set(false, true);
        return ack;
    }
    uint8_t readByte(bool ack) {
        uint8_t b = 0;
        for (int i = 0; i < 8; ++i) {
            set(false, true);
            set(true, true);
            b = static_cast<uint8_t>(b << 1 | (line() ? 1 : 0));
        }
        set(false, !ack);
        set(true, !ack);
        set(false, true);
        return b;
    }
};

void testEeprom() {
    // NBA Jam TE (Acclaim, 24C02): SCL on bit 0 of 0x200000, SDA on bit 0 of 0x200001.
    std::vector<uint8_t> rom = testrom::build();
    rom.resize(0x300000, 0xFF);
    const char serial[] = "GM T-81406 -00";
    std::copy(serial, serial + 14, rom.begin() + 0x180);
    Cartridge cart;
    cart.loadData(rom);
    check(cart.hasEeprom() && cart.saveRam().size() == 256 && !cart.header().hasSram, "EEPROM: found by serial, 256 bytes");
    I2cMaster m{cart, 0x200000, 0x200001, 0, 0};
    // The games' first check: SDA reads back what they put on it while the chip is idle.
    m.set(false, true);
    const bool high = m.line();
    m.set(false, false);
    check(high && !m.line(), "EEPROM: idle SDA follows the 68000");
    m.start();
    bool acks = m.sendByte(0xA0) && m.sendByte(0x10);
    for (uint8_t b : {0x53, 0x6E, 0x61}) acks = acks && m.sendByte(b);
    m.stop();
    check(acks && cart.saveRam()[0x10] == 0x53 && cart.saveRam()[0x12] == 0x61 && cart.hasSaveData(),
          "EEPROM: a page write lands at its word address");
    m.start();
    acks = m.sendByte(0xA0) && m.sendByte(0x11);
    m.start();  // repeated start, then read
    acks = acks && m.sendByte(0xA1);
    const uint8_t r1 = m.readByte(true), r2 = m.readByte(false);
    m.stop();
    check(acks && r1 == 0x6E && r2 == 0x61, "EEPROM: random read, then the next byte");
    // Word writes move both lines at once, as the game's start-up code does.
    cart.write16(0x200000, 0x0101);
    check(m.line(), "EEPROM: word write reaches both lines");

    // Sega's X24C01 (Wonder Boy in Monster World): both lines on 0x200001, 7-bit address.
    const char sega[] = "GM G-4060 -00";
    std::copy(sega, sega + 13, rom.begin() + 0x180);
    Cartridge ww;
    ww.loadData(rom);
    I2cMaster s{ww, 0x200001, 0x200001, 1, 0};
    s.start();
    acks = s.sendByte(0x05 << 1) && s.sendByte(0xC3);
    s.stop();
    s.start();
    acks = acks && s.sendByte(0x05 << 1 | 1);
    const uint8_t r = s.readByte(false);
    s.stop();
    check(ww.hasEeprom() && ww.saveRam().size() == 128 && acks && r == 0xC3, "EEPROM: X24C01 write and read");

    // Dodge Danpei joins its serial's words with _: "GM_00004076-00".
    const char danpei[] = "GM_00004076-00";
    std::copy(danpei, danpei + 14, rom.begin() + 0x180);
    Cartridge dd;
    dd.loadData(rom);
    check(dd.hasEeprom(), "EEPROM: found by a serial written with _ (Dodge Danpei)");
}

void testPads() {
    Io io;
    io.reset();
    io.setPadType(0, PadType::SixButton);
    PadState p;
    p.a = p.start = p.x = p.mode = p.up = true;
    io.setPad(0, p);
    uint64_t now = 1000;
    io.write(0xA10009, 0x40, now);  // TH is an output
    auto set = [&](bool th) {
        now += 100;
        io.write(0xA10003, th ? 0x40 : 0x00, now);
        return static_cast<uint8_t>(io.read(0xA10003, now) & 0x3F);
    };
    // Reads: TH=1 UDLRBC, TH=0 UD00AS, ... the third TH=0 reads 0000AS, then TH=1 ZYXM BC
    // and the fourth TH=0 1111AS. Buttons are active low.
    const uint8_t r1 = set(true), r2 = set(false);
    set(true);
    set(false);
    set(true);
    const uint8_t r6 = set(false), r7 = set(true), r8 = set(false);
    check(r1 == 0x3E, "6-button pad: TH high reads up, down, left, right, B, C");
    check(r2 == 0x02, "6-button pad: TH low reads up, down, A, Start");
    check(r6 == 0x00, "6-button pad: third TH low reads 0000 in the direction bits");
    check(r7 == 0x33, "6-button pad: then TH high reads Z, Y, X, Mode");
    check(r8 == 0x0F, "6-button pad: fourth TH low reads 1111");
    now += 200000;
    check((io.read(0xA10003, now) & 0x3F) == 0x02, "6-button pad: counter resets after a pause");

    io.setPadType(0, PadType::ThreeButton);
    set(true);
    set(false);
    set(true);
    set(false);
    set(true);
    check(set(false) == 0x02, "3-button pad never shows the extra buttons");

    // Trouble Shooter: TH written low, then made an input and read at once.
    now += 200000;
    io.write(0xA10003, 0x00, now);
    io.write(0xA10009, 0x00, now + 84);
    check((io.read(0xA10003, now + 224) & 0x3F) == 0x02, "TH let go while low still reads low for a moment");
    check((io.read(0xA10003, now + 2000) & 0x7F) == 0x7E, "TH let go floats high after that");

    // Nothing in the port: TH low reads all high, not a pad's left and right held low.
    io.setPadType(0, PadType::None);
    io.write(0xA10009, 0x40, now);
    check(set(true) == 0x3F && set(false) == 0x3F, "empty port: TH low reads no pad (ESPN NFL looks)");

    // softcheck's I/O port test: the serial registers keep what is written (S-Ctrl's
    // low three bits are status), RxData has nothing.
    io.write(0xA1000F, 0x5A, now);
    io.write(0xA10019, 0xFF, now);
    check(io.read(0xA1000F, now) == 0x5A && io.read(0xA10019, now) == 0xF8 && io.read(0xA10011, now) == 0x00 &&
              io.read(0xA1001B, now) == 0xFF,
          "I/O: serial TxData and S-Ctrl read back what was written (softcheck)");
}

void testPsg() {
    Psg psg;
    psg.setClock(Psg::kClockNtsc);
    psg.setSampleRate(44100);
    psg.write(0x80 | 0x0E);  // tone 0 period: 0x0FE -> ~440 Hz
    psg.write(0x0F);
    psg.write(0x90);         // tone 0 at full volume
    psg.setStereo(0xFF);
    psg.run(static_cast<int>(Psg::kClockNtsc / 10));
    const auto& s = psg.samples();
    check(s.size() / 2 >= 4405 && s.size() / 2 <= 4415, "PSG: a tenth of a second at 44.1 kHz");
    int crossings = 0;
    for (size_t i = 0; i + 2 < s.size(); i += 2)
        if ((s[i] > 0) != (s[i + 2] > 0)) ++crossings;
    check(crossings >= 86 && crossings <= 90, "PSG: tone 0 plays at about 440 Hz");
}

void testVdpPorts() {
    VdpMd vdp;
    vdp.reset();
    vdp.writeControl(0x8104);              // mode 5 (in mode 4 registers above 10 are ignored)
    vdp.writeControl(0x8F02);              // auto-increment 2
    vdp.writeControl(0x4000);              // VRAM write at 0
    vdp.writeControl(0x0000);
    vdp.writeData(0x1234);
    vdp.writeData(0xABCD);
    vdp.writeControl(0x0000);              // VRAM read at 0
    vdp.writeControl(0x0000);
    const uint16_t a = vdp.readData(), b = vdp.readData();
    check(a == 0x1234 && b == 0xABCD, "VDP: VRAM written and read back through the data port");
    vdp.writeControl(0xC000);              // CRAM write at 0
    vdp.writeControl(0x0000);
    vdp.writeData(0x0EEE);
    check(vdp.cram()[0] == 0x0EEE, "VDP: CRAM write");
    // DMA fill: 0x20 bytes at 0x1000 with 0x77.
    vdp.writeControl(0x8114);              // DMA enabled
    vdp.writeControl(0x8F01);
    vdp.writeControl(0x9320);
    vdp.writeControl(0x9400);
    vdp.writeControl(0x9780);              // fill
    vdp.writeControl(0x5000);              // VRAM write 0x1000 with DMA (CD5)
    vdp.writeControl(0x0080);
    vdp.writeData(0x7700);
    // The data word lands at 0x1000. The fill then runs over 0x1001..0x1020, writing its
    // byte each time to the other byte of the word: 0x1000, 0x1003, 0x1002, ... 0x1021.
    bool filled = vdp.vram()[0x1000] == 0x77 && vdp.vram()[0x1001] == 0x00;
    for (int i = 0x1002; i < 0x1020; ++i) filled = filled && vdp.vram()[static_cast<size_t>(i)] == 0x77;
    check(filled && vdp.vram()[0x1020] == 0 && vdp.vram()[0x1021] == 0x77, "VDP: DMA fill");
}

void testVdpFillScrollTable() {
    // Contra Hard Corps builds its line scroll table with two fills of auto-increment 4:
    // one from the even address of each entry, which sets the low bytes, and one from the
    // odd address, which sets the high bytes. Writing the fill byte to the address itself
    // swapped the halves and left the lower part of the screen still.
    VdpMd vdp;
    vdp.reset();
    vdp.writeControl(0x8174);  // display and DMA on
    vdp.writeControl(0x8F04);
    auto fill = [&](uint16_t addrCommand, uint16_t value) {
        vdp.writeControl(0x9400);
        vdp.writeControl(0x93E0);  // 224 lines
        vdp.writeControl(0x9780);
        vdp.writeControl(addrCommand);
        vdp.writeControl(0x0082);  // VRAM 0xB800 or 0xB801, with DMA
        vdp.writeData(value);
    };
    fill(0x7800, 0xB0B0);
    fill(0x7801, 0xFBFB);
    bool table = true;
    for (int line = 1; line < 224; ++line) {
        const size_t a = 0xB800 + static_cast<size_t>(line) * 4;
        table = table && vdp.vram()[a] == 0xFB && vdp.vram()[a + 1] == 0xB0;
    }
    check(table, "VDP: DMA fills build a scroll table byte by byte");
}

void testVdpDirectColor() {
    // With the display off the screen shows the backdrop colour as it is at each pixel:
    // colours written to it during the line show from where they land on.
    auto vdp = std::make_unique<VdpMd>();
    vdp->reset();
    vdp->writeControl(0x8104);  // display off
    vdp->writeControl(0x8C81);  // H40
    vdp->writeControl(0x8F00);
    vdp->writeControl(0x8700);  // backdrop: colour 0
    vdp->startLine(0, 0);
    vdp->writeControl(0xC000, 10);
    vdp->writeControl(0x0000, 20);
    vdp->writeData(0x000E, 400);   // red, from about pixel 50
    vdp->writeData(0x00E0, 1600);  // green, from about pixel 200
    vdp->sync(VdpMd::kActiveClocks);
    vdp->renderLine(0);
    const auto& f = vdp->frame();
    check(f.rgba[10] == 0xFF000000u && f.rgba[100] == 0xFFFF0000u && f.rgba[250] == 0xFF00FF00u,
          "VDP: backdrop colours written during a line with the display off");
}

void testVdpHblankFlag() {
    // Status bit 2 follows horizontal blanking, so code that polls it to time its writes
    // gets past the loop instead of waiting for ever.
    VdpMd vdp;
    vdp.reset();
    vdp.writeControl(0x8144);  // display on
    for (const bool h40 : {false, true}) {
        vdp.writeControl(h40 ? 0x8C81 : 0x8C00);
        auto hblank = [&](int clock) { return (vdp.readStatus(clock) & 0x04) != 0; };
        bool active = false;
        for (int c = 900; c < VdpMd::kActiveClocks; c += 50) active = active || hblank(c);
        const bool blank = hblank(VdpMd::kActiveClocks + 400) && hblank(VdpMd::kMasterPerLine - 1);
        check(!active && blank && !hblank(VdpMd::kActiveClocks),
              h40 ? "VDP: HBlank status flag (H40)" : "VDP: HBlank status flag (H32)");
    }
}

void testVdpSpriteLimit() {
    // H40 draws 20 sprites and 320 sprite pixels per line. Without the limit the rest are
    // drawn too, but the status bits stay what the console would give: the game reads the
    // same overflow, and sprites past the limit never set the collision bit.
    auto run = [](bool unlimited, int wideCells, int step, int count, int overlapFrom) {
        auto vdp = std::make_unique<VdpMd>();
        vdp->reset();
        vdp->setUnlimitedSprites(unlimited);
        vdp->writeControl(0x8144);  // display on
        vdp->writeControl(0x8C81);  // H40
        vdp->writeControl(0x8578);  // sprite table at 0xF000
        vdp->writeControl(0x8F02);
        auto at = [&](uint32_t addr) {
            vdp->writeControl(static_cast<uint16_t>(0x4000 | (addr & 0x3FFF)));
            vdp->writeControl(static_cast<uint16_t>(addr >> 14));
        };
        at(0x20);  // tile 1: colour 1 everywhere
        for (int i = 0; i < 16; ++i) vdp->writeData(0x1111);
        vdp->writeControl(0xC002);  // CRAM colour 1
        vdp->writeControl(0x0000);
        vdp->writeData(0x0EEE);
        at(0xF000);
        for (int n = 0; n < count; ++n) {
            const int x = n >= overlapFrom ? 300 : 128 + n * step;
            vdp->writeData(128 + 10);                                              // y: line 10
            vdp->writeData(static_cast<uint16_t>(((wideCells - 1) << 10) | (n + 1 < count ? n + 1 : 0)));
            vdp->writeData(0x0001);                                                // tile 1
            vdp->writeData(static_cast<uint16_t>(x));
        }
        vdp->readStatus(0);
        vdp->renderLine(10);
        const uint16_t status = vdp->readStatus(0);
        return std::make_pair(std::move(vdp), status);
    };
    auto sprite = [](const VdpMd& v, int x) { return v.frame().info[10 * VideoFrameMd::kMaxWidth + x].layer == MdLayer::Sprite; };

    // 30 one-cell sprites, the last ten on top of each other: past the 20 per line.
    auto [limited, s1] = run(false, 1, 8, 30, 20);
    auto [free, s2] = run(true, 1, 8, 30, 20);
    check(sprite(*limited, 19 * 8) && !sprite(*limited, 300 - 128), "VDP: 20 sprites per line in H40");
    check(sprite(*free, 300 - 128), "VDP: without the limit the 21st sprite is drawn");
    check(s1 == s2 && (s1 & 0x40) && !(s1 & 0x20),
          "VDP: without the limit the overflow bit is the same and sprites past it do not collide");

    // 12 sprites four cells wide, 16 pixels apart (only their first cell has pixels): the
    // 11th starts past the 320 pixels, so its first cell at x = 160 stays empty.
    auto [narrow, s3] = run(false, 4, 16, 12, 99);
    auto [wide, s4] = run(true, 4, 16, 12, 99);
    check(sprite(*narrow, 9 * 16) && !sprite(*narrow, 160) && !sprite(*narrow, 178),
          "VDP: 320 sprite pixels per line in H40");
    check(sprite(*wide, 160) && sprite(*wide, 178) && s3 == s4 && (s3 & 0x40),
          "VDP: past the 320 pixels without the limit, same status");
}

void testVdpSpriteMaskAfterDotOverflow() {
    // A sprite at x = 0 masks the sprites after it once one at another x came first, or
    // from the start when the line before ran out of sprite pixels (Nemesis' sprite
    // masking test, "mask S1 on dot overflow").
    auto run = [](bool overflowAbove) {
        auto vdp = std::make_unique<VdpMd>();
        vdp->reset();
        vdp->writeControl(0x8144);  // display on, H32
        vdp->writeControl(0x8578);  // sprite table at 0xF000
        vdp->writeControl(0x8F02);
        auto at = [&](uint32_t addr) {
            vdp->writeControl(static_cast<uint16_t>(0x4000 | (addr & 0x3FFF)));
            vdp->writeControl(static_cast<uint16_t>(addr >> 14));
        };
        at(0x20);  // tile 1: colour 1 everywhere
        for (int i = 0; i < 16; ++i) vdp->writeData(0x1111);
        at(0xF000);
        int n = 0;
        auto sprite = [&](int y, int cells, int x, bool last) {
            vdp->writeData(static_cast<uint16_t>(128 + y));
            vdp->writeData(static_cast<uint16_t>(((cells - 1) << 10) | (last ? 0 : n + 1)));
            vdp->writeData(0x0001);
            vdp->writeData(static_cast<uint16_t>(x));
            ++n;
        };
        // Lines 10-17: 9 sprites of 32 pixels, past the 256 of H32 (or 7, within them).
        for (int i = 0; i < (overflowAbove ? 9 : 7); ++i) sprite(10, 4, 128 + i * 8, false);
        sprite(18, 1, 0, false);         // from line 18: the mask first,
        sprite(18, 1, 128 + 100, true);  // then a sprite it hides or not
        vdp->renderLine(17);
        vdp->renderLine(18);
        const auto& f = vdp->frame();
        return f.sprites[18 * VideoFrameMd::kMaxWidth + 100] != 0;
    };
    check(!run(true) && run(false), "VDP: a sprite at x = 0 masks from the start after a line out of sprite pixels");
}

void testOverclock() {
    // A 68000 loop adding 1 to a long in RAM: addq.l #1,($FF0000).l; bra.s back. At 200%
    // it goes round about twice as often in the same frames.
    std::vector<uint8_t> rom(0x400, 0);
    const uint8_t vectors[] = {0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x02, 0x00};
    const uint8_t loop[] = {0x52, 0xB9, 0x00, 0xFF, 0x00, 0x00, 0x60, 0xF8};
    std::copy(std::begin(vectors), std::end(vectors), rom.begin());
    std::copy(std::begin(loop), std::end(loop), rom.begin() + 0x200);
    auto counter = [](const MegaDrive& md) {
        const auto& ram = md.workRam();
        return static_cast<uint32_t>(ram[0]) << 24 | ram[1] << 16 | ram[2] << 8 | ram[3];
    };
    auto run = [&](int percent) {
        MegaDrive md;
        md.loadRomData(rom);
        md.setOverclock(percent);
        for (int i = 0; i < 10; ++i) md.runFrame();
        return counter(md);
    };
    const uint32_t normal = run(100), fast = run(200), fastest = run(400);
    const double ratio = static_cast<double>(fast) / normal, ratio4 = static_cast<double>(fastest) / normal;
    check(normal > 1000 && ratio > 1.95 && ratio < 2.05 && ratio4 > 3.9 && ratio4 < 4.1,
          "overclock: the 68000 runs 2 and 4 times as much");
    {
        MegaDrive md;
        md.setOverclock(1000);
        const int high = md.overclock();
        md.setOverclock(10);
        check(high == MegaDrive::kMaxOverclock && md.overclock() == 100, "overclock: kept within 100-400%");
    }

    // It is part of the state (a netplay guest starts from the host's), and a state
    // without it is from a console at its real speed.
    MegaDrive a, b;
    a.loadRomData(rom);
    b.loadRomData(rom);
    a.setOverclock(300);
    b.setOverclock(200);
    for (int i = 0; i < 3; ++i) a.runFrame();
    std::vector<uint8_t> fastState, plainState;
    std::string error;
    a.saveState(fastState);
    check(b.loadState(fastState, &error) && b.overclock() == 300, "overclock: the state carries it");
    for (int i = 0; i < 5; ++i) {
        a.runFrame();
        b.runFrame();
    }
    check(counter(a) == counter(b), "overclock: the same run after loading it");
    a.setOverclock(100);
    a.saveState(plainState);
    check(b.loadState(plainState, &error) && b.overclock() == 100, "overclock: a state without it loads at 100%");
}

void testBusAckOpenBus() {
    // Indy Heat asks for the Z80's bus and waits with tst.b ($A11100).l; beq.s back, which
    // only gets out through the bits besides BUSACK: they read the 68000's prefetch (the
    // beq's opcode), as on the console. Then it writes 1 to $FF0000.
    std::vector<uint8_t> rom(0x400, 0);
    const uint8_t vectors[] = {0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x02, 0x00};
    const uint8_t code[] = {0x33, 0xFC, 0x01, 0x00, 0x00, 0xA1, 0x11, 0x00,   // move.w #$100,$A11100
                            0x33, 0xFC, 0x01, 0x00, 0x00, 0xA1, 0x12, 0x00,   // move.w #$100,$A11200
                            0x4A, 0x39, 0x00, 0xA1, 0x11, 0x00, 0x67, 0xF8,   // tst.b $A11100; beq.s
                            0x13, 0xFC, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x00,   // move.b #1,$FF0000
                            0x60, 0xFE};
    std::copy(std::begin(vectors), std::end(vectors), rom.begin());
    std::copy(std::begin(code), std::end(code), rom.begin() + 0x200);
    MegaDrive md;
    md.loadRomData(rom);
    md.runFrame();
    check(md.workRam()[0] == 1, "BUSACK: the bits besides bit 0 read the open bus (Indy Heat)");
}

void testPrefetch() {
    // misc_test's "M68K prefetch": code in RAM rewrites the instruction right after the one
    // writing, which the 68000 has fetched already, so the old one (moveq #1,d1) runs.
    std::vector<uint8_t> rom(0x400, 0);
    const uint8_t vectors[] = {0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x02, 0x00};
    const uint8_t code[] = {0x41, 0xF9, 0x00, 0xFF, 0x00, 0x2C,              // lea $FF002C,a0
                            0x21, 0x3C, 0x72, 0x01, 0x4E, 0x75,              // move.l #$72014E75,-(a0)
                            0x21, 0x3C, 0x30, 0xBC, 0x72, 0x00,              // move.l #$30BC7200,-(a0)
                            0x21, 0x3C, 0x41, 0xFA, 0x00, 0x06,              // move.l #$41FA0006,-(a0)
                            0x4E, 0x90,                                      // jsr (a0)
                            0x13, 0xC1, 0x00, 0xFF, 0x00, 0x00,              // move.b d1,$FF0000
                            0x60, 0xFE};
    std::copy(std::begin(vectors), std::end(vectors), rom.begin());
    std::copy(std::begin(code), std::end(code), rom.begin() + 0x200);
    MegaDrive md;
    md.loadRomData(rom);
    md.runFrame();
    check(md.workRam()[0] == 1 && md.workRam()[0x28] == 0x72 && md.workRam()[0x29] == 0x00,
          "68000: an instruction written over right after being fetched runs as it was (misc_test)");
}

void testZ80ResetKeepsRegisters() {
    // misc_test: the Z80's /RESET leaves its registers alone (only power sets them).
    MegaDrive md;
    md.loadRomData(testrom::build());
    md.z80().sp = 0x1234;
    md.z80().bc.w = 0x5678;
    md.z80().pc = 0x100;
    md.z80().reset();
    check(md.z80().sp == 0x1234 && md.z80().bc.w == 0x5678 && md.z80().pc == 0, "Z80: /RESET keeps the registers (misc_test)");
    md.z80().powerOn();
    check(md.z80().sp == 0xFFFF && md.z80().bc.w == 0xFFFF, "Z80: power sets them to $FFFF");
}

void testTraceSkipsIllegal() {
    // m68k_opcode_sizes turns tracing on with ori.w #$8000,sr and then runs a line F word:
    // an instruction that cannot be run takes only its own exception, not a trace after it.
    std::vector<uint8_t> rom(0x400, 0);
    const uint8_t vectors[] = {0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x02, 0x00};
    const uint8_t code[] = {0x00, 0x7C, 0x80, 0x00,  // ori.w #$8000,sr
                            0xF0, 0x00};             // line F
    const uint8_t lineF[] = {0x13, 0xFC, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x00, 0x60, 0xFE};  // move.b #1,$FF0000
    const uint8_t trace[] = {0x13, 0xFC, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x01, 0x60, 0xFE};  // move.b #1,$FF0001
    std::copy(std::begin(vectors), std::end(vectors), rom.begin());
    rom[0x26] = 0x03;  // trace at 0x310
    rom[0x27] = 0x10;
    rom[0x2E] = 0x03;  // line F at 0x300
    std::copy(std::begin(code), std::end(code), rom.begin() + 0x200);
    std::copy(std::begin(lineF), std::end(lineF), rom.begin() + 0x300);
    std::copy(std::begin(trace), std::end(trace), rom.begin() + 0x310);
    MegaDrive md;
    md.loadRomData(rom);
    md.runFrame();
    check(md.workRam()[0] == 1 && md.workRam()[1] == 0, "68000: a line F word under trace is not traced (m68k_opcode_sizes)");
}

void testVintFlagWait() {
    // Mega-Lo-Mania (Tyrants) turns the VINT on and then polls the VDP status for its flag:
    // only the instruction under way when the VINT comes can read it, before the 68000
    // takes the interrupt (whose acknowledge clears it). Then it writes 1 to $FF0000.
    std::vector<uint8_t> rom(0x400, 0);
    const uint8_t vectors[] = {0x00, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x02, 0x00};
    const uint8_t code[] = {0x33, 0xFC, 0x81, 0x64, 0x00, 0xC0, 0x00, 0x04,   // move.w #$8164,$C00004
                            0x46, 0xFC, 0x25, 0x00,                           // move #$2500,sr
                            0x30, 0x39, 0x00, 0xC0, 0x00, 0x04,               // move.w $C00004,d0
                            0x08, 0x00, 0x00, 0x07, 0x67, 0xF4,               // btst #7,d0; beq.s back
                            0x13, 0xFC, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x00,   // move.b #1,$FF0000
                            0x60, 0xFE};
    std::copy(std::begin(vectors), std::end(vectors), rom.begin());
    rom[0x7A] = 0x03;  // VINT at 0x300: rte
    rom[0x300] = 0x4E;
    rom[0x301] = 0x73;
    std::copy(std::begin(code), std::end(code), rom.begin() + 0x200);
    MegaDrive md;
    md.loadRomData(rom);
    for (int i = 0; i < 10; ++i) md.runFrame();
    check(md.workRam()[0] == 1, "68000: the instruction under way when the VINT comes reads its flag (Mega-Lo-Mania)");
}

void testSplitRoms() {
    // QuackShot (Rev A): 512 KB, its header ending at 0x13FFFF; the second 256 KB at 0x100000.
    std::vector<uint8_t> rom(0x80000, 0x11);
    std::fill(rom.begin() + 0x40000, rom.end(), 0x22);
    rom[0x1A4] = 0x00, rom[0x1A5] = 0x13, rom[0x1A6] = 0xFF, rom[0x1A7] = 0xFF;
    Cartridge quack;
    quack.loadData(rom);
    check(quack.read8(0x100000) == 0x22 && quack.read8(0x13FFFF) == 0x22 && quack.read8(0x3FFFF) == 0x11,
          "QuackShot: the second ROM chip answers at 0x100000");

    // Triple Play: 3 MB ending at 0x3FFFFF with RAM at 0x200001; the last MB at 0x300000.
    rom.assign(0x300000, 0x11);
    std::fill(rom.begin() + 0x200000, rom.end(), 0x33);
    rom[0x1A4] = 0x00, rom[0x1A5] = 0x3F, rom[0x1A6] = 0xFF, rom[0x1A7] = 0xFF;
    const uint8_t ram[] = {'R', 'A', 0xF8, 0x20, 0x00, 0x20, 0x00, 0x01, 0x00, 0x20, 0xFF, 0xFF};
    std::copy(std::begin(ram), std::end(ram), rom.begin() + 0x1B0);
    Cartridge triple;
    triple.loadData(rom);
    triple.write8(0x200001, 0x5A);
    check(triple.read8(0x300000) == 0x33 && triple.read8(0x3FFFFF) == 0x33 && triple.read8(0x200001) == 0x5A,
          "Triple Play: the last MB at 0x300000, the RAM at 0x200000 without switching it on");
}

void testVdpWidescreen() {
    // 16:9 adds 53 columns on each side in H40 (42 in H32), drawn from the plane and the
    // sprites past the edges. The console's own 320 columns stay as they were. Each side
    // can have its own width.
    auto render = [](int left, int right, bool h40) {
        auto vdp = std::make_unique<VdpMd>();
        vdp->reset();
        vdp->setWidescreen(left, right);
        vdp->writeControl(0x8144);                // display on
        vdp->writeControl(h40 ? 0x8C81 : 0x8C00);  // H40 or H32
        vdp->writeControl(0x8230);                // plane A at 0xC000
        vdp->writeControl(0x8578);                // sprite table at 0xF000
        vdp->writeControl(0x9001);                // 64x32 planes
        vdp->writeControl(0x8F02);
        auto at = [&](uint32_t addr) {
            vdp->writeControl(static_cast<uint16_t>(0x4000 | (addr & 0x3FFF)));
            vdp->writeControl(static_cast<uint16_t>(addr >> 14));
        };
        at(0x20);  // tile 1: colour 1 everywhere
        for (int i = 0; i < 16; ++i) vdp->writeData(0x1111);
        vdp->writeControl(0xC002);  // CRAM colour 1
        vdp->writeControl(0x0000);
        vdp->writeData(0x0EEE);
        at(0xC000 + 63 * 2);  // plane A: tile 1 in the last column of row 0 only (x 504..511)
        vdp->writeData(0x0001);
        at(0xF000);  // one sprite with its left edge 16 pixels past the right of the screen
        vdp->writeData(128);
        vdp->writeData(0x0000);
        vdp->writeData(0x0001);
        vdp->writeData(static_cast<uint16_t>(128 + (h40 ? 320 : 256) + 16));
        vdp->renderLine(0);
        return vdp;
    };
    auto layer = [](const VdpMd& v, int x) { return v.frame().info[x].layer; };

    auto off = render(0, 0, true);
    check(off->frame().width == 320 && off->frame().wideLeft == 0 && off->frame().wideRight == 0,
          "VDP widescreen: off by default");
    check(VdpMd::wideColumnsFor(16, 9) == 53 && VdpMd::wideColumnsFor(16, 10) == 32, "VDP widescreen: 16:9 and 16:10");
    auto wide = render(53, 53, true);
    const VideoFrameMd& f = wide->frame();
    check(f.width == 426 && f.wideLeft == 53 && f.wideRight == 53, "VDP widescreen: 16:9 in H40 is 426 pixels, 53 on each side");
    // Plane x 504..511 is screen x -8..-1: only the widescreen columns show it.
    check(layer(*off, 0) == MdLayer::Backdrop && layer(*wide, 53 - 8) == MdLayer::PlaneA &&
              layer(*wide, 53 - 1) == MdLayer::PlaneA && layer(*wide, 53) == MdLayer::Backdrop,
          "VDP widescreen: the plane past the left edge");
    check(layer(*wide, 53 + 336) == MdLayer::Sprite && layer(*wide, 53 + 335) == MdLayer::Backdrop,
          "VDP widescreen: a sprite past the right edge");
    bool same = true;
    for (int x = 0; x < 320; ++x) same &= off->frame().rgba[x] == f.rgba[53 + x];
    check(same, "VDP widescreen: the console's own picture is the same");

    auto narrow = render(53, 53, false);
    check(narrow->frame().width == 340 && narrow->frame().wideLeft == 42 && layer(*narrow, 42 + 272) == MdLayer::Sprite,
          "VDP widescreen: 16:9 in H32 is 340 pixels");
    // Only the left side: the plane past the left edge shows, the sprite past the right does not.
    auto leftOnly = render(50, 0, true);
    const VideoFrameMd& g = leftOnly->frame();
    check(g.width == 370 && g.wideLeft == 50 && g.wideRight == 0 && layer(*leftOnly, 50 - 8) == MdLayer::PlaneA &&
              g.consoleWidth() == 320,
          "VDP widescreen: only the left side");
    bool sameLeft = true;
    for (int x = 0; x < 320; ++x) sameLeft &= off->frame().rgba[x] == g.rgba[50 + x];
    check(sameLeft, "VDP widescreen: one side only keeps the console's picture");
}

void testTestRom() {
    MegaDrive md;
    md.loadRomData(testrom::build());
    md.setSampleRate(44100);
    std::vector<int16_t> sound;
    for (int i = 0; i < 60; ++i) {
        md.runFrame();
        sound.insert(sound.end(), md.audio().samples().begin(), md.audio().samples().end());
        md.audio().clearSamples();
    }
    const VideoFrameMd& f = md.frame();
    check(f.width == 320 && f.height == 224, "test ROM: H40, 224 lines");
    // A block of the "P" (plane A, high priority), the floor, a ball and plane B.
    const MdPixel& block = f.info[44 * VideoFrameMd::kMaxWidth + 20];
    check(block.layer == MdLayer::PlaneA && block.priority, "test ROM: plane A tile with priority");
    check(f.info[200 * VideoFrameMd::kMaxWidth + 100].layer == MdLayer::PlaneA, "test ROM: the floor");
    check(f.info[(84 + 8) * VideoFrameMd::kMaxWidth + 60 + 8].layer == MdLayer::Sprite, "test ROM: a sprite");
    check(f.info[150 * VideoFrameMd::kMaxWidth + 10].layer == MdLayer::PlaneB, "test ROM: plane B");
    // Ball 0 has no priority and sits on the "P": hidden in 2D, still in the sprite layer.
    const int i0 = (60 + 8) * VideoFrameMd::kMaxWidth + 24 + 8;
    check(f.info[i0].layer == MdLayer::PlaneA && (f.sprites[i0] >> 24), "test ROM: low sprite behind a high tile");
    // Plane B scrolls one pixel a frame from the VBlank interrupt.
    check(f.lines[100].hscrollB != 0, "test ROM: VBlank interrupt scrolls plane B");
    double sumSq = 0;
    for (int16_t s : sound) sumSq += static_cast<double>(s) * s;
    const double rms = std::sqrt(sumSq / static_cast<double>(std::max<size_t>(1, sound.size())));
    check(sound.size() / 2 > 43000 && sound.size() / 2 < 45000, "test ROM: a second of sound at 44.1 kHz");
    check(rms > 500, "test ROM: the YM2612 plays its tone");
}

void testStates() {
    MegaDrive md;
    md.loadRomData(testrom::build());
    md.setSampleRate(44100);
    for (int i = 0; i < 30; ++i) md.runFrame();
    md.audio().clearSamples();
    std::vector<uint8_t> state;
    check(md.saveState(state) && state.size() > 100, "savestate: saved");

    auto run = [&](int frames, std::vector<uint32_t>& hashes, std::vector<int16_t>& sound) {
        PadState p;
        for (int i = 0; i < frames; ++i) {
            p.right = (i / 7) % 2 == 0;  // some input, the same both times
            md.setPads(p, PadState{});
            md.runFrame();
            hashes.push_back(frameHash(md.frame()));
            sound.insert(sound.end(), md.audio().samples().begin(), md.audio().samples().end());
            md.audio().clearSamples();
        }
    };
    std::vector<uint32_t> h1, h2;
    std::vector<int16_t> s1, s2;
    run(90, h1, s1);
    std::string error;
    check(md.loadState(state, &error), "savestate: loaded");
    run(90, h2, s2);
    check(h1 == h2, "savestate: the same frames after loading");
    check(soundHash(s1) == soundHash(s2) && s1.size() == s2.size(), "savestate: the same sound after loading");

    std::vector<uint8_t> again;
    md.loadState(state, &error);
    md.saveState(again);
    check(again == state, "savestate: save, load, save gives the same bytes");

    std::vector<uint32_t> pixels;
    int tw = 0, th = 0;
    check(MegaDrive::stateThumbnail(state, pixels, &tw, &th) && tw == 160 && th == 112, "savestate: thumbnail");

    // Damaged files are rejected and leave the console as it was.
    md.loadState(state, &error);
    std::vector<uint8_t> before;
    md.saveState(before);
    bool allRejected = true;
    std::vector<uint8_t> bad(state.begin(), state.begin() + static_cast<long>(state.size() / 2));
    allRejected &= !md.loadState(bad, &error);
    bad = {'P', 'M', 'D', 'S', 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
    allRejected &= !md.loadState(bad, &error);
    std::mt19937 rng(1234);
    int loaded = 0;
    for (int i = 0; i < 200; ++i) {
        bad = state;
        for (int k = 0; k < 4; ++k) bad[12 + rng() % (bad.size() - 12)] ^= static_cast<uint8_t>(1 + rng() % 255);
        if (md.loadState(bad, &error)) {
            ++loaded;
            md.loadState(state, &error);
        }
    }
    std::vector<uint8_t> after;
    md.saveState(after);
    check(allRejected && after == before, "savestate: truncated or random files are rejected safely");
    std::printf("     (%d of 200 randomly damaged states still decompressed and loaded)\n", loaded);

    MegaDrive other;
    std::vector<uint8_t> rom = testrom::build();
    rom[0x150] = 'X';  // another title, another CRC
    other.loadRomData(rom);
    check(!other.loadState(state, &error) && error.find("otra ROM") != std::string::npos,
          "savestate: a state from another ROM is rejected");
}

void testPlanes() {
    MegaDrive md;
    md.loadRomData(testrom::build());
    for (int i = 0; i < 20; ++i) md.runFrame();
    const VideoFrameMd& f = md.frame();
    for (const bool highB : {false, true}) {
        Planes planes;
        splitPlanes(f, SplitOptions{highB, false}, planes);
        // Golden rule: looking straight at the planes, front to back, shows the 2D picture.
        bool same = planes.width == f.width && planes.height == f.height;
        bool skyFull = true;
        for (int y = 0; y < planes.height && same; ++y)
            for (int x = 0; x < planes.width; ++x) {
                const size_t o = static_cast<size_t>(y) * planes.width + x;
                uint32_t seen = 0;
                for (int p = kPlanes - 1; p >= 0 && !seen; --p)
                    if (planes[p][o] >> 24) seen = planes[p][o];
                if ((seen & 0xFFFFFF) != (f.pixel(x, y) & 0xFFFFFF)) same = false;
                skyFull = skyFull && (planes[PlaneSky][o] >> 24);
            }
        check(same, highB ? "planes: straight on = 2D (high B in front)" : "planes: straight on = 2D");
        check(skyFull, "planes: the sky has no holes");
    }
    Planes planes;
    splitPlanes(f, SplitOptions{}, planes);
    const size_t ball0 = static_cast<size_t>(60 + 8) * 320 + 24 + 8;
    check(planes[PlaneSprites][ball0] != 0, "planes: a sprite behind a priority tile is kept");
    check(planes[PlanePriority][static_cast<size_t>(44) * 320 + 20] != 0, "planes: priority tiles in front");
    std::vector<uint32_t> solid;
    markSolidTiles(f, planes, solid);
    check(solid[static_cast<size_t>(210) * 320 + 100] != 0, "planes: the floor is a solid tile");
    const size_t floor = static_cast<size_t>(210) * 320 + 100, sky = static_cast<size_t>(150) * 320 + 10;
    const bool hiddenB = planes[PlaneB][floor] != 0;
    clearBehindSolid(solid, planes);
    check(hiddenB && planes[PlaneB][floor] == 0 && planes[PlaneB][sky] != 0,
          "planes: plane B is cleared inside solid tiles, kept elsewhere");
}

void testEdgeDistance() {
    constexpr int kW = 64, kH = 48;
    std::vector<uint32_t> plane(kW * kH, 0);
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 30; ++x) plane[y * kW + x] = 0xFF123456u;
    markEdgeDistance(plane, kW, kH);
    auto distance = [&](int x, int y) { return kMaxEdgeDistance - (255 - int(plane[y * kW + x] >> 24)) / 16; };
    check(distance(10, 10) == 1 && distance(29, 20) == 1, "edge distance: 1 on the outline");
    check(distance(11, 20) == 2 && distance(20, 12) == 3, "edge distance: grows inwards");
    check(distance(20, 20) == kMaxEdgeDistance, "edge distance: capped in the middle");
    check(plane[0] == 0 && (plane[20 * kW + 20] & 0xFFFFFF) == 0x123456, "edge distance: colours untouched");
}

void testPal() {
    MegaDrive md;
    md.setRegion(Region::EuropePal);
    md.loadRomData(testrom::build());
    md.runFrame();
    check(md.pal() && std::abs(md.refreshRate() - 49.70) < 0.01, "PAL: 50 Hz");
    check((md.peek(0xA10001) & 0xC0) == 0xC0, "PAL: version register says overseas and PAL");
}

// The SH-2 on a flat 64 KB of big-endian RAM: loops, delayed branches (with a
// PC-relative load in the slot), the step division, interrupts, the division unit,
// the DMA controller and the serial link between two CPUs.
struct FlatSh2Bus : Sh2Bus {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000);
    uint32_t sh2Read(Sh2&, uint32_t addr, int size) override {
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) v = v << 8 | ram[(addr + static_cast<uint32_t>(i)) & 0xFFFF];
        return v;
    }
    void sh2Write(Sh2&, uint32_t addr, uint32_t value, int size) override {
        for (int i = 0; i < size; ++i)
            ram[(addr + static_cast<uint32_t>(i)) & 0xFFFF] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
    }
    void put16(uint32_t a, std::initializer_list<uint16_t> ops) {
        for (uint16_t op : ops) {
            ram[a & 0xFFFF] = static_cast<uint8_t>(op >> 8);
            ram[(a + 1) & 0xFFFF] = static_cast<uint8_t>(op);
            a += 2;
        }
    }
    void put32(uint32_t a, uint32_t v) { put16(a, {static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v)}); }
};

void testSh2() {
    FlatSh2Bus bus;
    Sh2 cpu(bus, 0);
    bus.put32(0, 0x100);
    bus.put32(4, 0x1000);
    bus.put16(0x100, {0xE00A, 0xE100,  // MOV #10,R0; MOV #0,R1
                      0x310C, 0x4010,  // loop: ADD R0,R1; DT R0
                      0x8BFC,          // BF loop
                      0xB011,          // BSR sub
                      0xE207,          // (slot) MOV #7,R2
                      0xD304,          // MOV.L @(0x120),R3
                      0x001B});        // SLEEP
    bus.put32(0x120, 0x12345678);
    bus.put16(0x130, {0x7201, 0x000B, 0xC701});  // sub: ADD #1,R2; RTS; (slot) MOVA @(4,PC),R0
    cpu.reset();
    for (int i = 0; i < 200 && !cpu.sleeping(); ++i) cpu.step();
    check(cpu.sleeping() && cpu.reg(1) == 55, "SH-2: bucle con DT y BF");
    check(cpu.reg(2) == 8, "SH-2: BSR y RTS con sus huecos de retardo");
    check(cpu.reg(0) == 0x114, "SH-2: MOVA en un hueco de retardo usa el destino del salto");
    check(cpu.reg(3) == 0x12345678, "SH-2: MOV.L relativo al PC");

    // 1000 / 7 with the step division of the manual.
    bus.put32(0, 0x200);
    {
        uint32_t a = 0x200;
        bus.put16(a, {0x9116, 0xE007, 0x4028, 0x0019});  // MOV.W @(0x230),R1; MOV #7,R0; SHLL16 R0; DIV0U
        a += 8;
        for (int i = 0; i < 16; ++i, a += 2) bus.put16(a, {0x3104});  // DIV1 R0,R1
        bus.put16(a, {0x4124, 0x611D, 0x001B});  // ROTCL R1; EXTU.W R1,R1; SLEEP
        bus.put16(0x230, {1000});                // the word MOV.W reads
    }
    cpu.reset();
    for (int i = 0; i < 200 && !cpu.sleeping(); ++i) cpu.step();
    check(cpu.reg(1) == 142, "SH-2: división paso a paso con DIV0U y DIV1");

    // An external interrupt at level 12 takes vector 70 and RTE comes back.
    bus.put32(0, 0x300);
    bus.put32(70 * 4, 0x380);
    bus.put16(0x300, {0xE000, 0x400E, 0xAFFE, 0x0009});  // MOV #0,R0; LDC R0,SR; loop: BRA loop; NOP
    bus.put16(0x380, {0xE501, 0x002B, 0x0009});          // MOV #1,R5; RTE; NOP
    cpu.reset();
    for (int i = 0; i < 4; ++i) cpu.step();
    cpu.setIrl(12);
    cpu.step();
    const bool entered = cpu.pc() == 0x380 && ((cpu.sr() >> 4) & 15) == 12;
    cpu.setIrl(0);
    cpu.step();
    cpu.step();
    check(entered && cpu.reg(5) == 1 && cpu.pc() == 0x304 && ((cpu.sr() >> 4) & 15) == 0,
          "SH-2: interrupción externa y vuelta con RTE");

    // The division unit.
    cpu.write(0xFFFFFF00, 7, 4);
    cpu.write(0xFFFFFF04, static_cast<uint32_t>(-100), 4);
    check(static_cast<int32_t>(cpu.read(0xFFFFFF04, 4)) == -14 && static_cast<int32_t>(cpu.read(0xFFFFFF10, 4)) == -2,
          "SH-2: unidad de división de 32 bits");

    // A DMA copy of eight words, requested by the controller itself.
    for (int i = 0; i < 16; ++i) bus.ram[0x400 + i] = static_cast<uint8_t>(i + 1);
    cpu.write(0xFFFFFF80, 0x400, 4);
    cpu.write(0xFFFFFF84, 0x500, 4);
    cpu.write(0xFFFFFF88, 8, 4);
    cpu.write(0xFFFFFF8C, 0x5601, 4);
    cpu.write(0xFFFFFFB0, 1, 4);
    cpu.step();
    check(std::equal(bus.ram.begin() + 0x400, bus.ram.begin() + 0x410, bus.ram.begin() + 0x500) &&
              (cpu.read(0xFFFFFF8C, 4) & 2),
          "SH-2: copia por DMA");

    // A byte through the serial link to the other CPU.
    Sh2 other(bus, 1);
    cpu.setPeer(&other);
    other.setPeer(&cpu);
    other.write(0xFFFFFE02, 0x10, 1);  // receiver on
    cpu.write(0xFFFFFE02, 0x20, 1);    // transmitter on
    cpu.write(0xFFFFFE03, 0x42, 1);
    cpu.write(0xFFFFFE04, 0x7F, 1);  // clear TDRE: send
    cpu.write(0xFFFFFFB0, 0, 4);
    cpu.step();
    check(other.read(0xFFFFFE05, 1) == 0x42 && (other.read(0xFFFFFE04, 1) & 0x40), "SH-2: puerto serie entre las dos CPU");
}

// A 32X program built by hand: the 68000 turns the adapter on and waits for both SH-2s
// to say they are ready; then the master SH-2 draws a gradient in direct colour mode
// and swaps the frame buffers. Checks the boot without a BIOS, the communication
// words, the frame buffer, the VDP and the layer drawn over the Mega Drive's picture.
// A 32X cartridge around an SH-2 program loaded at 0x06000000: the 68000 turns the
// adapter on, waits for both SH-2s to say they are ready and writes 1 in the seventh
// communication word.
std::vector<uint8_t> marsRom(const uint8_t* program, size_t size, uint32_t masterEntry, uint32_t slaveEntry) {
    std::vector<uint8_t> rom(0x800 + ((size + 3) & ~size_t{3}), 0);
    std::copy(program, program + size, rom.begin() + 0x800);
    auto put16 = [&](uint32_t a, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            rom[a] = static_cast<uint8_t>(w >> 8);
            rom[a + 1] = static_cast<uint8_t>(w);
            a += 2;
        }
    };
    auto put32 = [&](uint32_t a, uint32_t v) { put16(a, {static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v)}); };
    put32(0, 0xFFFF00);
    put32(4, 0x3F0);
    const char* system = "SEGA 32X        ";
    std::copy(system, system + 16, rom.begin() + 0x100);
    put32(0x3D4, 0x800);  // SH-2 program in the ROM
    put32(0x3D8, 0);      // to the start of SDRAM
    put32(0x3DC, static_cast<uint32_t>((size + 3) & ~size_t{3}));
    put32(0x3E0, masterEntry);
    put32(0x3E4, slaveEntry);
    put32(0x3E8, 0x06000000);  // VBRs
    put32(0x3EC, 0x06000000);
    put16(0x3F0, {0x33FC, 0x0003, 0x00A1, 0x5100,          // move.w #3,$a15100   adapter on, SH-2s running
                  0x0CB9, 0x4D5F, 0x4F4B, 0x00A1, 0x5120,  // cmp.l #'M_OK',$a15120
                  0x66F4,                                  // bne.s *-10
                  0x0CB9, 0x535F, 0x4F4B, 0x00A1, 0x5124,  // cmp.l #'S_OK',$a15124
                  0x66F4,                                  // bne.s *-10
                  0x33FC, 0x0001, 0x00A1, 0x512C,          // move.w #1,$a1512c   tell the master to go
                  0x60FE});                                // bra.s *
    return rom;
}

void testMars() {
    std::vector<uint8_t> program(0x200, 0);
    auto put16 = [&](uint32_t a, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            program[a] = static_cast<uint8_t>(w >> 8);
            program[a + 1] = static_cast<uint8_t>(w);
            a += 2;
        }
    };
    auto put32 = [&](uint32_t a, uint32_t v) { put16(a, {static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v)}); };
    const uint32_t m = 0;  // SDRAM 0x06000000
    put16(m + 0x100, {0xD117,           // mov.l #$20004000,r1
                      0xD218,           // mov.l #$8000,r2
                      0x2121,           // mov.w r2,@r1        FM: the SH-2s get the VDP
                      0xD318,           // mov.l #$2000402c,r3
                      0x6031,           // wait: mov.w @r3,r0
                      0x8801, 0x8BFC,   // cmp/eq #1,r0; bf wait
                      0xD417,           // mov.l #$24000000,r4  frame buffer
                      0xD519,           // mov.l #224,r5
                      0xE601, 0x4618,   // mov #1,r6; shll8 r6  every line starts at word $100
                      0x2461, 0x7402,   // lines: mov.w r6,@r4; add #2,r4
                      0x4510, 0x8BFB,   // dt r5; bf lines
                      0xD414,           // mov.l #$24000200,r4
                      0xD516,           // mov.l #320,r5
                      0xE700,           // mov #0,r7
                      0x2471, 0x7402,   // pixels: mov.w r7,@r4; add #2,r4
                      0x7701,           // add #1,r7
                      0x4510, 0x8BFA,   // dt r5; bf pixels
                      0xD811,           // mov.l #$20004100,r8
                      0xE002, 0x2801,   // mov #2,r0; mov.w r0,@r8   direct colour
                      0xE001, 0x8185,   // mov #1,r0; mov.w r0,@(10,r8)  show the other buffer
                      0xAFFE, 0x0009});
    put32(m + 0x160, 0x20004000);
    put32(m + 0x164, 0x8000);
    put32(m + 0x168, 0x2000402C);
    put32(m + 0x16C, 0x24000000);
    put32(m + 0x170, 0x24000200);
    put32(m + 0x174, 0x20004100);
    put32(m + 0x178, 224);
    put32(m + 0x17C, 320);
    put16(m + 0x180, {0xAFFE, 0x0009});  // the slave just waits

    MegaDrive md;
    md.loadRomData(marsRom(program.data(), program.size(), 0x06000100, 0x06000180));
    check(md.is32x(), "32X: se reconoce el cartucho por su cabecera");
    for (int i = 0; i < 6; ++i) md.runFrame();
    const auto& comm = md.mars()->comm();
    check(comm[0] == 0x4D5F && comm[1] == 0x4F4B && comm[2] == 0x535F && comm[3] == 0x4F4B,
          "32X: las dos SH-2 arrancan sin BIOS y dicen M_OK y S_OK");
    const VideoFrameMd& f = md.frame();
    bool drawn = true;
    for (int x : {0, 1, 31, 32, 200, f.width - 1}) {  // the VDP was left in H32: 256 pixels
        const int i = 100 * VideoFrameMd::kMaxWidth + x;
        auto expand = [](uint32_t v) { return (v << 3) | (v >> 2); };
        const uint32_t want = 0xFF000000u | expand(x & 31) << 16 | expand((x >> 5) & 31) << 8 | expand((x >> 10) & 31);
        if (f.info[i].layer != MdLayer::Mars || f.rgba[i] != want || f.mars[i] != want) drawn = false;
    }
    check(drawn, "32X: la SH-2 dibuja en el frame buffer y se ve sobre el fondo de la Mega Drive");

    // Savestates keep the 32X: the next frame is the same after a round trip.
    std::vector<uint8_t> state;
    md.saveState(state);
    md.runFrame();
    const uint32_t a = frameHash(md.frame());
    std::string error;
    check(md.loadState(state, &error), "32X: carga su estado");
    md.runFrame();
    check(frameHash(md.frame()) == a && md.mars()->comm()[0] == 0x4D5F, "32X: el estado guarda y recupera la 32X");
}

// The reset button with a 32X: the adapter stays on, so the 68000 starts again from the
// vector ROM (at 0x880200, the game's hot start), and the SH-2s are not reset but get
// the VRES interrupt (the master's handler writes "RE" in the last communication word).
void testMarsResetButton() {
    std::vector<uint8_t> program(0x200, 0);
    auto put16 = [&](uint32_t a, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            program[a] = static_cast<uint8_t>(w >> 8);
            program[a + 1] = static_cast<uint8_t>(w);
            a += 2;
        }
    };
    auto put32 = [&](uint32_t a, uint32_t v) { put16(a, {static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v)}); };
    put16(0x100, {0xE000, 0x400E, 0xAFFE, 0x0009});  // master: mov #0,r0; ldc r0,sr; bra *
    put16(0x140, {0xAFFE, 0x0009});                  // slave: bra *
    put32(0x11C, 0x06000180);                         // VRES: vector 71
    put16(0x180, {0xD102,                             // mov.l #$2000402e,r1
                  0xD003,                             // mov.l #'RE',r0
                  0x2101,                             // mov.w r0,@r1
                  0xAFFE, 0x0009});                   // bra *
    put32(0x18C, 0x2000402E);
    put32(0x190, 0x5245);
    std::vector<uint8_t> rom = marsRom(program.data(), program.size(), 0x06000100, 0x06000140);
    static constexpr uint16_t kHotStart[] = {0x33FC, 0x4854, 0x00A1, 0x512A,  // move.w #'HT',$a1512a
                                             0x60FE};                         // bra.s *
    for (size_t i = 0; i < std::size(kHotStart); ++i) {
        rom[0x200 + i * 2] = static_cast<uint8_t>(kHotStart[i] >> 8);
        rom[0x201 + i * 2] = static_cast<uint8_t>(kHotStart[i]);
    }

    MegaDrive md;
    md.loadRomData(rom);
    for (int i = 0; i < 6; ++i) md.runFrame();
    const auto& comm = md.mars()->comm();
    check(comm[2] == 0x535F && comm[5] == 0 && comm[7] == 0, "32X reset: arranca en frío");
    md.reset();
    for (int i = 0; i < 2; ++i) md.runFrame();
    uint16_t a15100 = 0;
    md.mars()->read68k(0xA15100, a15100);
    check((a15100 & 3) == 3, "32X reset: el adaptador sigue encendido y las SH-2 en marcha (ADEN, RES)");
    check(comm[5] == 0x4854, "32X reset: el 68000 vuelve a empezar desde la ROM de vectores (0x880200)");
    check(comm[7] == 0x5245, "32X reset: la SH-2 maestra recibe la interrupción VRES");
    check(comm[2] == 0x535F, "32X reset: las palabras de comunicación se mantienen");
}

// The C program in tests/mars, compiled for the SH-2: both CPUs run the same checks
// (libgcc's division, 64-bit and floating point arithmetic, jump tables...) and must
// get what this machine gets; then VBlank interrupts, packed pixels through the
// palette, and a square wave on the PWM.
namespace marschecks {
#include "mars/checks.inc"
}

void testMarsProgram() {
    MegaDrive md;
    md.setSampleRate(44100);
    md.loadRomData(marsRom(kMarsProgram, sizeof(kMarsProgram), 0x06000200, 0x06000220));
    for (int i = 0; i < 30 && md.mars()->comm()[7] != 0xD0; ++i) md.runFrame();
    uint32_t want[16];
    marschecks::runChecks(want);
    const auto& sdram = md.mars()->sdram();
    auto result = [&](int i) {
        const size_t a = 0x3F000 + static_cast<size_t>(i) * 4;
        return static_cast<uint32_t>(sdram[a] << 24 | sdram[a + 1] << 16 | sdram[a + 2] << 8 | sdram[a + 3]);
    };
    int masterOk = 0, slaveOk = 0;
    for (int i = 0; i < 16; ++i) {
        if (result(i) == want[i]) ++masterOk;
        else std::printf("     maestra[%d] = %08x, debería ser %08x\n", i, result(i), want[i]);
        if (result(16 + i) == want[i]) ++slaveOk;
    }
    check(masterOk == 16, "32X: la SH-2 maestra calcula lo mismo que este ordenador (programa en C)");
    check(slaveOk == 16, "32X: la SH-2 esclava también");
    check(result(32) >= 3, "32X: interrupciones de VBlank con su manejador y RTE");
    check(md.mars()->comm()[7] == 0xD0, "32X: el programa termina");

    // What only changes the timing (the caches, a CPU parked in a spin loop) goes into
    // the savestate too: the frames after a round trip are the same, to the clock.
    for (int at : {1, 2, 3}) {
        MegaDrive copy;
        copy.setSampleRate(44100);
        copy.loadRomData(marsRom(kMarsProgram, sizeof(kMarsProgram), 0x06000200, 0x06000220));
        for (int i = 0; i < at; ++i) copy.runFrame();
        std::vector<uint8_t> state;
        copy.saveState(state);
        auto run = [&] {
            for (int i = 0; i < 4; ++i) copy.runFrame();
            return std::make_tuple(frameHash(copy.frame()), copy.mars()->sdram(), copy.mars()->master().pc(),
                                   copy.mars()->slave().pc(), copy.mars()->master().reg(0), copy.mars()->slave().reg(0));
        };
        const auto first = run();
        std::string error;
        check(copy.loadState(state, &error) && run() == first,
              ("32X: el estado guarda la caché y los bucles de espera (tras " + std::to_string(at) + " fotogramas)").c_str());
    }

    md.runFrame();
    md.runFrame();
    const VideoFrameMd& f = md.frame();
    bool packed = true;
    for (int x : {0, 7, 8, 100, f.width - 1}) {
        const uint32_t i = static_cast<uint32_t>(50 * VideoFrameMd::kMaxWidth + x);
        const uint32_t level = static_cast<uint32_t>(x) >> 3;
        const uint32_t c = (level << 3) | (level >> 2);
        if (f.info[i].layer != MdLayer::Mars || f.rgba[i] != (0xFF000000u | c << 16 | c << 8 | c)) packed = false;
    }
    check(packed, "32X: píxeles empaquetados a través de la paleta");

    md.audio().clearSamples();
    for (int i = 0; i < 5; ++i) md.runFrame();
    const auto& out = md.audio().samples();
    int lo = 0, hi = 0;
    for (size_t i = 0; i < out.size(); i += 2) {
        lo = std::min<int>(lo, out[i]);
        hi = std::max<int>(hi, out[i]);
    }
    check(out.size() > 2 * 3000 && hi - lo > 10000, "32X: el PWM suena");
}

void testCdImage() {
    std::vector<CdImage::Track> t;
    std::string error;
    const std::string one =
        "FILE \"game.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n    INDEX 00 00:10:00\n    INDEX 01 00:12:00\n";
    bool ok = CdImage::parseCue(one, {1200ull * 2352}, t, &error) && t.size() == 2;
    ok = ok && t[0].begin == -150 && t[0].start == 0 && t[0].end == 749 && t[0].dataBegin == 0 && !t[0].audio;
    ok = ok && t[1].begin == 750 && t[1].start == 900 && t[1].end == 1199 && t[1].dataBegin == 750 &&
         t[1].offset == 750ull * 2352 && t[1].audio;
    check(ok, "CD: hoja .cue de un archivo, con INDEX 00 en la pista de audio");

    const std::string two =
        "REM una pista por archivo\nFILE \"t1.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n"
        "FILE \"t2.bin\" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n";
    ok = CdImage::parseCue(two, {100ull * 2048, 50ull * 2352}, t, &error) && t.size() == 2;
    ok = ok && t[0].end == 99 && t[0].sectorBytes == 2048 && t[1].file == 1 && t[1].begin == 100 &&
         t[1].start == 250 && t[1].dataBegin == 250 && t[1].end == 299 && t[1].offset == 0;
    check(ok, "CD: hoja .cue de varios archivos, con PREGAP");
    check(!CdImage::parseCue("FILE \"a.bin\" BINARY\n  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n", {2336}, t,
                             &error),
          "CD: un tipo de pista que no se lee se dice");

    // The same two-file disc on disk, read back.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "plumsg_cd_test";
    std::filesystem::create_directories(dir);
    std::vector<uint8_t> data(100 * 2048), audio(50 * 2352);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i / 2048 + i);
    for (size_t i = 0; i < audio.size(); ++i) audio[i] = static_cast<uint8_t>(i * 7);
    std::ofstream(dir / "t1.bin", std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
    std::ofstream(dir / "t2.bin", std::ios::binary).write(reinterpret_cast<const char*>(audio.data()), audio.size());
    std::ofstream(dir / "disco.cue") << two;
    CdImage disc;
    ok = disc.open((dir / "disco.cue").u8string(), &error);
    check(ok, "CD: abre una hoja .cue con sus archivos");
    if (ok) {
        uint8_t sector[2352];
        disc.readSector(5, sector);
        const bool header = sector[0] == 0 && sector[1] == 0xFF && sector[11] == 0 && sector[12] == 0 &&
                            sector[13] == 0x02 && sector[14] == 0x05 && sector[15] == 1;
        check(header && std::memcmp(sector + 16, &data[5 * 2048], 2048) == 0,
              "CD: un sector de datos de 2048 bytes sale con su cabecera");
        disc.readSector(251, sector);
        check(std::memcmp(sector, &audio[2352], 2352) == 0, "CD: un sector de audio sale tal cual");
        disc.readSector(120, sector);
        check(std::all_of(sector, sector + 2352, [](uint8_t b) { return b == 0; }), "CD: el PREGAP es silencio");
        check(disc.trackAt(-150) == 0 && disc.trackAt(100) == 1 && disc.trackAt(300) == -1 && disc.leadOut() == 300,
              "CD: pistas y final del disco");
    }
    disc.close();
    std::filesystem::remove_all(dir);

    // A .chd made with chdman from a data track and an audio track whose pregap is in
    // the file (tests/megacd/disco.chd; the bytes follow the formulas below).
    ok = disc.open(std::string(PLUMSG_TEST_DIR) + "/megacd/disco.chd", &error);
    check(ok, "CD: abre un .chd");
    if (ok) {
        const auto& tr = disc.tracks();
        check(tr.size() == 2 && tr[0].end == 49 && !tr[0].audio && tr[1].begin == 50 && tr[1].start == 200 &&
                  tr[1].end == 299 && tr[1].audio,
              "CD: las pistas del .chd, con el pregap guardado");
        bool same = true;
        uint8_t sector[2352];
        for (int32_t lba = 0; lba < 50 && same; ++lba) {
            disc.readSector(lba, sector);
            for (int j = 0; j < 2048; ++j) same = same && sector[16 + j] == static_cast<uint8_t>(lba * 7 + j);
            same = same && sector[15] == 1 && sector[14] == static_cast<uint8_t>((lba + 150) % 75 / 10 << 4 | (lba + 150) % 75 % 10);
        }
        for (int32_t lba = 50; lba < 300 && same; ++lba) {
            disc.readSector(lba, sector);
            const int i = lba - 50;
            for (int j = 0; j < 2352; ++j) same = same && sector[j] == static_cast<uint8_t>(i * 13 + j * 3);
        }
        check(same, "CD: los sectores del .chd salen como los del .bin original");
    }
}

// A disc made in memory for the Mega CD program: a data track whose sector 0 is a
// disc header, and a second, audio track with a loud square wave.
// `header` is the region in its header and `security` the byte of its security code
// that says the region the BIOS checks (0x7A USA, 0xA1 Japan, 0x64 Europe).
std::unique_ptr<CdImage> testDisc(char header = 'U', uint8_t security = 0x7A) {
    std::vector<CdImage::Track> tracks(2);
    tracks[0].number = 1;
    tracks[0].begin = -150;
    tracks[0].start = tracks[0].dataBegin = 0;
    tracks[0].end = tracks[0].dataEnd = 39;
    tracks[0].sectorBytes = 2048;
    tracks[1].number = 2;
    tracks[1].audio = true;
    tracks[1].begin = 40;
    tracks[1].start = tracks[1].dataBegin = 190;
    tracks[1].end = tracks[1].dataEnd = 389;
    tracks[1].offset = 40 * 2048;
    std::vector<uint8_t> data(40 * 2048 + 200 * 2352);
    std::memcpy(data.data(), "SEGADISCSYSTEM  ", 16);
    std::memcpy(&data[0x100], "SEGA MEGA DRIVE ", 16);
    data[0x1F0] = static_cast<uint8_t>(header);
    data[0x20B] = security;
    for (size_t i = 40 * 2048; i < data.size(); i += 4) {
        const int16_t v = ((i / 400) & 1) ? 12000 : -12000;
        for (int c = 0; c < 2; ++c) {
            data[i + c * 2] = static_cast<uint8_t>(v);
            data[i + c * 2 + 1] = static_cast<uint8_t>(v >> 8);
        }
    }
    auto disc = std::make_unique<CdImage>();
    disc->openTracks(tracks, data);
    return disc;
}

void testMegaCd() {
    MegaDrive md;
    md.setSampleRate(44100);
    std::string error;
    check(!md.loadDiscImage(testDisc(), &error) && error.find("BIOS") != std::string::npos,
          "Mega CD: sin BIOS, dice cuál falta");
    std::vector<uint8_t> bios(kMegaCdProgram, kMegaCdProgram + sizeof kMegaCdProgram);
    bios.resize(0x20000);
    md.setCdBios(Region::UsaNtsc, bios);
    check(md.loadDiscImage(testDisc(), &error) && md.isMegaCd(), "Mega CD: carga un disco con la BIOS de su región");
    {
        // The region comes from the security code, and the console follows the disc.
        MegaDrive other;
        other.setCdBios(Region::UsaNtsc, bios);
        other.setRegion(Region::EuropePal);
        check(other.loadDiscImage(testDisc('J', 0x7A), &error) && other.region() == Region::UsaNtsc,
              "Mega CD: la región sale del código de seguridad (Snatcher dice J en un disco americano)");
        check(!other.loadDiscImage(testDisc('U', 0x64), &error) && error.find("Europa") != std::string::npos,
              "Mega CD: un disco europeo pide la BIOS europea");
    }
    MegaCd& cd = *md.megaCd();
    auto result = [&](int i) {
        const auto& prg = cd.programRam();
        const size_t a = 0x7F000 + static_cast<size_t>(i) * 2;
        return static_cast<uint16_t>(prg[a] << 8 | prg[a + 1]);
    };
    // Run until the PCM plays, measure it, then until the program ends.
    int pcmLo = 0, pcmHi = 0, frames = 0;
    for (; frames < 600 && cd.status()[7] != 0xD0; ++frames) {
        md.audio().clearSamples();
        md.runFrame();
        if (cd.status()[6] == 0x9C)
            for (size_t i = 0; i < md.audio().samples().size(); i += 2) {
                pcmLo = std::min<int>(pcmLo, md.audio().samples()[i]);
                pcmHi = std::max<int>(pcmHi, md.audio().samples()[i]);
            }
    }
    check(cd.status()[0] == 0x1234 && result(0) == 1, "Mega CD: la CPU secundaria arranca y habla con la principal");
    check(result(1) >= 3, "Mega CD: interrupciones del temporizador");
    check(result(2) == 1, "Mega CD: interrupción de nivel 2 desde la CPU principal");
    check(result(9) == 0x5A, "Mega CD: memoria de guardado");
    check(result(3) == 0x0102, "Mega CD: la unidad dice la primera y la última pista");
    check(result(4) == 0x5345 && result(5) == 0x4741 && result(10) > 0,
          "Mega CD: lee el sector 0 del disco por el controlador");
    check(result(6) == 0x8000, "Mega CD: la transferencia termina");
    check(result(7) == 0x5345, "Mega CD: DMA del controlador a la memoria de palabras");
    check(result(8) == 0x5555 && result(11) == 1, "Mega CD: el chip gráfico dibuja un sello");
    check(md.workRam()[0] == 0x60 && md.workRam()[1] == 0x0D && md.workRam()[2] == 0x55 && md.workRam()[3] == 0x55,
          "Mega CD: la memoria de palabras vuelve a la CPU principal");
    check(pcmHi - pcmLo > 4000, "Mega CD: el chip PCM suena");
    check(cd.status()[7] == 0xD0, "Mega CD: el programa termina");

    // CD audio: track 2 plays once the drive has got there.
    int lo = 0, hi = 0;
    for (int i = 0; i < 150; ++i) {
        md.audio().clearSamples();
        md.runFrame();
        for (size_t j = 0; j < md.audio().samples().size(); j += 2) {
            lo = std::min<int>(lo, md.audio().samples()[j]);
            hi = std::max<int>(hi, md.audio().samples()[j]);
        }
    }
    check(hi - lo > 8000, "Mega CD: suena el audio de la pista 2");

    // Savestates with the Mega CD in them come back exactly.
    std::vector<uint8_t> state;
    check(md.saveState(state), "Mega CD: guarda el estado");
    md.audio().clearSamples();
    md.runFrame();
    const uint32_t sound = soundHash(md.audio().samples());
    const uint32_t pc = cd.sub().pc;
    std::string err;
    check(md.loadState(state, &err), "Mega CD: carga el estado");
    md.audio().clearSamples();
    md.runFrame();
    check(soundHash(md.audio().samples()) == sound && cd.sub().pc == pc, "Mega CD: tras cargar el estado sigue igual");

    // In 2 Mbit mode the sub CPU waits for word RAM it has given to the console, and makes
    // the access once it is back (Marko's Magic Football writes right after handing it over).
    static const uint8_t kWait[] = {
        0x13, 0xFC, 0x00, 0x01, 0x00, 0xFF, 0x80, 0x03,  // move.b #1,$FF8003 (RET: to the console)
        0x33, 0xFC, 0x12, 0x34, 0x00, 0x08, 0x00, 0x00,  // move.w #$1234,$080000
        0x33, 0xFC, 0x00, 0x01, 0x00, 0x07, 0xF1, 0x00,  // move.w #1,$07F100
        0x60, 0xFE};                                     // bra.s *
    std::copy(std::begin(kWait), std::end(kWait), cd.programRam().begin() + 0x70000);
    cd.programRam()[0x7F100] = cd.programRam()[0x7F101] = 0;
    cd.wordRam()[0] = 0;
    cd.sub().pc = 0x70000;
    cd.sub().sr = 0x2700;
    md.runFrame();
    md.runFrame();
    check(cd.wordRam()[0] == 0 && cd.programRam()[0x7F101] == 0 && cd.sub().pc == 0x70008,
          "Mega CD: la CPU secundaria espera la memoria de palabras que tiene la principal");
    cd.write68k(0xA12002, 0x0002, false, true);  // DMNA: the console gives it back
    md.runFrame();
    check(cd.wordRam()[0] == 0x1234 && cd.programRam()[0x7F101] == 1,
          "Mega CD: y escribe en cuanto se la devuelve");
}

// MSU-MD: a cartridge that boots with the Mega CD attached (mode 1) and plays CD tracks.
// Paprium's chip RAM: 68000 DMA from the first 64 KB finishes at once with setInstantDma,
// other DMA still holds the CPU for its slots.
void testVdpInstantDma() {
    auto run = [](bool instant, uint32_t source, bool* landed) {
        auto vdp = std::make_unique<VdpMd>();
        vdp->reset();
        vdp->setDmaReader([](uint32_t addr) { return static_cast<uint16_t>(addr); });
        vdp->setInstantDma(instant);
        vdp->writeControl(0x8174);  // display and DMA on
        vdp->writeControl(0x8C81);
        vdp->writeControl(0x8F02);
        vdp->startLine(10, 0);
        const uint16_t regs[] = {0x9300, 0x9404,  // 0x400 words
                                 static_cast<uint16_t>(0x9500 | ((source >> 1) & 0xFF)),
                                 static_cast<uint16_t>(0x9600 | ((source >> 9) & 0xFF)),
                                 static_cast<uint16_t>(0x9700 | ((source >> 17) & 0x7F))};
        for (uint16_t r : regs) vdp->writeControl(r, 100);
        vdp->writeControl(0x4000, 200);  // VRAM 0, with DMA
        const int stall = vdp->writeControl(0x0080, 210);
        *landed = vdp->vram()[0x7FE] == static_cast<uint8_t>((source + 0x7FE) >> 8) &&
                  vdp->vram()[0x7FF] == static_cast<uint8_t>(source + 0x7FE);
        return stall;
    };
    bool landedChip = false, landedSlow = false, landedRam = false;
    const int chip = run(true, 0x2000, &landedChip);
    const int slow = run(false, 0x2000, &landedSlow);
    const int ram = run(true, 0xFF0000, &landedRam);
    check(chip == 0 && landedChip && slow > 100000 && !landedSlow && ram > 100000,
          "VDP: DMA from a cartridge chip's RAM done at once, other DMA in the VDP's time");
}

// Paprium's chip, on a made-up ROM with its header: the status words, a block unpacked
// and read out at 0xC000, and a save bank written and read back.
void testSvp() {
    // A cartridge with the SVP's "SV" and a DSP program at word 0x400.
    std::vector<uint8_t> rom(0x100000, 0);
    rom[0x1C8] = 'S';
    rom[0x1C9] = 'V';
    const uint16_t program[] = {
        0x0830, 0x1234,                  // ldi A, 1234h
        0x8800, 0x0001,                  // add A, 1
        0x00B3,                          // ld XST, A: to the 68000
        0x08E0, 0x0010, 0x08E0, 0x0818,  // PMC: DRAM word 10h, step 1
        0x00C0,                          // ld PM4, -: PM4 writes there
        0x00C3, 0x00C3,                  // ld PM4, A twice
        0x0810, 0x0003, 0x0820, 0xFFFC,  // ldi X, 3; ldi Y, -4
        0x0037,                          // ld A, P: A = X * Y * 2
        0x00CF,                          // ld PM4, AL
        0x4C00, 0x0412,                  // bra here
    };
    for (size_t i = 0; i < std::size(program); ++i) {
        rom[0x800 + i * 2] = static_cast<uint8_t>(program[i] >> 8);
        rom[0x801 + i * 2] = static_cast<uint8_t>(program[i]);
    }
    Cartridge cart;
    cart.loadData(rom);
    check(Svp::detect(cart), "svp: found by its header");
    Svp svp(cart);
    svp.powerOn();
    svp.runUntil(1000);
    check(svp.pc() == 0x412, "svp: the program ran to its loop");
    uint16_t v = 0;
    check(svp.read68k(0xA15004, v) && (v & 1), "svp: status says the DSP wrote XST");
    check(svp.read68k(0xA15004, v) && !(v & 1), "svp: reading the status takes that bit");
    check(svp.read68k(0xA15000, v) && v == 0x1235, "svp: XST holds the word");
    uint16_t a = 0, b = 0, c = 0;
    svp.read68k(0x300020, a);
    svp.read68k(0x300022, b);
    svp.read68k(0x300024, c);
    check(a == 0x1235 && b == 0x1235 && c == 0xFFE8, "svp: PM4 writes DRAM, stepping; P = X * Y * 2");
    check(svp.dmaRead(0x300022, v) && v == 0x1235 && svp.dmaRead(0x300026, v) && v == 0xFFE8,
          "svp: a VDP DMA reads one word behind");
    svp.write68k(0x300026, 0xAB00, true, false);
    check(svp.dram()[0x13] == 0xAB00, "svp: the 68000 writes DRAM");
}

void testPaprium() {
    std::vector<uint8_t> rom(0x100000, 0);
    std::memcpy(&rom[0x180], "GM T-574120", 11);
    const uint8_t ra[12] = {'R', 'A', 0xF8, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0xFF, 0xFF};
    std::memcpy(&rom[0x1B0], ra, sizeof ra);
    // Three literals, a run of four, a copy of the first three, the end.
    const uint8_t packed[] = {0x80, 0x03, 'A', 'B', 'C', 0x44, 'Z', 0x83, 7, 0x00};
    std::memcpy(&rom[0x20000], packed, sizeof packed);
    rom[0x8000] = 0x12;
    Cartridge cart;
    cart.loadData(rom);
    check(Paprium::detect(cart), "paprium: found by its header");
    Paprium chip(cart);
    chip.reset();
    check(chip.read16(0x1FE4) == 0xFFBB && chip.read16(0x1FE6) == 0xBCFF && chip.read16(0x1FEA) == 0x7FFF,
          "paprium: status words");
    check(chip.read16(0x1560) == 0x4EF9 && chip.read16(0x1564) == 0x0100 && chip.read8(0x8000) == 0x12,
          "paprium: RAM starts as the ROM, sent past the decoy");

    chip.write16(0x1E10, 0);
    chip.write16(0x1E12, 0x0002);
    chip.write16(0x1E14, 0x0000);
    chip.write16(0x1FEA, 0xDA02);
    chip.read16(0xC000);
    std::string out;
    for (uint32_t i = 0; i < 10; ++i) out += static_cast<char>(chip.read8(0xC000 + i));
    check(out == "ABCZZZZABC", "paprium: block unpacked and read out");

    for (uint32_t i = 0; i < 0x780; ++i) chip.write8(0x3000 + i, static_cast<uint8_t>(i * 7));
    chip.write16(0x1E12, 0x3000);
    chip.write16(0x1FEA, 0xE002);
    chip.write16(0x1E10, 0x5000);
    chip.write16(0x1FEA, 0xDF02);
    bool same = cart.hasSaveData() && cart.saveRamByte(0x780) == 0;
    for (uint32_t i = 0; i < 0x780; ++i) same = same && chip.read8(0x5000 + i) == static_cast<uint8_t>(i * 7);
    check(same, "paprium: a save bank written and read back");

    chip.write16(0x1FEA, 0x8400);
    check(chip.takeRestart() && !chip.takeRestart(), "paprium: command 0x84 starts the game again");

    // The music: a .wav in the folder, the album's track 2, which the chip calls 1.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "plumsg_paprium_test";
    std::filesystem::create_directories(dir);
    {
        std::vector<uint8_t> wav = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                    1, 0, 2, 0, 0xC0, 0x5D, 0, 0, 0, 0x77, 1, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
        const uint32_t frames = 24000, bytes = frames * 4;
        for (int i = 0; i < 4; ++i) wav.push_back(static_cast<uint8_t>(bytes >> (i * 8)));
        for (uint32_t i = 0; i < frames; ++i) {
            const int16_t l = 10000, r = -10000;
            wav.insert(wav.end(), {static_cast<uint8_t>(l), static_cast<uint8_t>(l >> 8), static_cast<uint8_t>(r),
                                   static_cast<uint8_t>(r >> 8)});
        }
        std::ofstream(dir / "02 Acid Dub.wav", std::ios::binary).write(reinterpret_cast<const char*>(wav.data()),
                                                                       static_cast<std::streamsize>(wav.size()));
    }
    check(chip.setMusicFolder(dir) == 1, "paprium: the music's files found by their numbers");
    chip.write16(0x1FEA, 0xCAFF);
    chip.write16(0x1FEA, 0xC9FF);
    chip.write16(0x1FEA, 0x8C01);
    bool playing = false;
    for (int tries = 0; tries < 2000 && !playing; ++tries) {
        chip.samples().clear();
        chip.runAudio(480, 48000);
        playing = chip.samples().back() != 0;
        if (!playing) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto& music = chip.samples();
    check(playing && std::abs(music[music.size() - 2] - 9921) < 50 && std::abs(music.back() + 9921) < 50,
          "paprium: the music plays from its file");
    chip.write16(0x1FEA, 0x8D00);
    chip.samples().clear();
    chip.runAudio(480, 48000);
    check(chip.samples().back() == 0, "paprium: and stops");
    std::filesystem::remove_all(dir);
}

// A program made to stand in for the Mega CD's BIOS (KRIKzz's mcd-verificator), loaded as
// a cartridge: it goes in the BIOS's place, at 0, and the cartridge slot stays empty.
void testMegaCdBootRom() {
    std::vector<uint8_t> rom(0x20000, 0);
    std::memcpy(&rom[0x120], "CDX BOOT ROM", 12);
    std::memcpy(&rom[0x180], "BR 00000000-00", 14);
    rom[0x200] = 0x12;
    rom[0x201] = 0x34;
    MegaDrive md;
    md.loadRomData(rom);
    check(md.isMegaCd() && !md.hasMsuMd() && md.peek16(0x200) == 0x1234,
          "Mega CD: una ROM de arranque hace de BIOS");
    check(md.peek(0x400001) == 0xFF, "Mega CD: sin cartucho no hay cartucho de RAM");
}

void testMsuMd() {
    std::vector<uint8_t> bios(kMegaCdProgram, kMegaCdProgram + sizeof kMegaCdProgram);
    bios.resize(0x20000);
    const std::vector<uint8_t> cart(kMegaCdCartridge, kMegaCdCartridge + sizeof kMegaCdCartridge);

    MegaDrive md;
    md.setSampleRate(44100);
    md.setCdBios(Region::EuropePal, bios);  // a cartridge takes a BIOS of any region
    md.setRegion(Region::UsaNtsc);
    md.loadRomDataWithDisc(cart, testDisc());
    check(md.hasMsuMd() && md.region() == Region::UsaNtsc, "MSU-MD: el cartucho arranca con el Mega CD, en su región");
    check(md.peek16(0x200) == 0x46FC && md.peek16(0x400100) == 0x5345 && md.peek16(0x400102) == 0x4741,
          "MSU-MD: el cartucho en 0, la BIOS en 0x400000");
    MegaCd& cd = *md.megaCd();
    int frames = 0;
    for (; frames < 600 && cd.status()[7] != 0xD0; ++frames) md.runFrame();
    for (int i = 0; i < 10; ++i) md.runFrame();
    const auto& prg = cd.programRam();
    check(cd.status()[0] == 0x1234 && prg[0x7F001] == 1,
          "MSU-MD: el cartucho carga el programa de la CPU secundaria en 0x420000");
    check(md.workRam()[0] == 0x60 && md.workRam()[1] == 0x0D && md.workRam()[2] == 0x55 && md.workRam()[3] == 0x55,
          "MSU-MD: la memoria de palabras en 0x600000");
    int lo = 0, hi = 0;
    for (int i = 0; i < 150; ++i) {
        md.audio().clearSamples();
        md.runFrame();
        for (size_t j = 0; j < md.audio().samples().size(); j += 2) {
            lo = std::min<int>(lo, md.audio().samples()[j]);
            hi = std::max<int>(hi, md.audio().samples()[j]);
        }
    }
    check(hi - lo > 8000, "MSU-MD: suena la pista de audio del CD");

    // On disk: Juego.md with Juego.cue next to it.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "plumsg_msu_test";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "Juego.md", std::ios::binary).write(reinterpret_cast<const char*>(cart.data()), cart.size());
    std::ofstream(dir / "Juego.cue") << "FILE \"Juego.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n";
    std::vector<uint8_t> iso(40 * 2048);
    std::ofstream(dir / "Juego.iso", std::ios::binary).write(reinterpret_cast<const char*>(iso.data()), iso.size());
    std::ofstream(dir / "Disco.bin", std::ios::binary).write(reinterpret_cast<const char*>(iso.data()), iso.size());
    std::ofstream(dir / "Disco.cue") << "FILE \"Disco.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n";
    std::ofstream(dir / "Solo.md", std::ios::binary).write(reinterpret_cast<const char*>(cart.data()), cart.size());
    check(msuMdCue((dir / "Juego.md").u8string()) == (dir / "Juego.cue").u8string() &&
              msuMdCue((dir / "Solo.md").u8string()).empty() && msuMdCue((dir / "Disco.bin").u8string()).empty(),
          "MSU-MD: el .cue con el nombre del cartucho, no el de un disco");
    std::string error;
    MegaDrive withBios;
    withBios.setCdBios(Region::JapanNtsc, bios);
    check(withBios.loadRom((dir / "Juego.md").u8string(), &error) && withBios.hasMsuMd() &&
              withBios.loadNotice().empty(),
          "MSU-MD: se reconoce al abrir el cartucho");
    MegaDrive noBios;
    check(noBios.loadRom((dir / "Juego.md").u8string(), &error) && !noBios.isMegaCd() &&
              noBios.loadNotice().find("BIOS") != std::string::npos,
          "MSU-MD: sin BIOS el cartucho va solo y dice por qué no hay música en CD");
    check(withBios.loadRom((dir / "Solo.md").u8string(), &error) && !withBios.isMegaCd(),
          "MSU-MD: un cartucho sin .cue va solo");
    std::filesystem::remove_all(dir);
}

}  // namespace

void testRewindBuffer() {
    RewindBuffer r;
    r.setLimits(3, 1000);
    for (uint8_t i = 1; i <= 5; ++i) r.push(std::vector<uint8_t>(10, i));
    std::vector<uint8_t> s;
    check(r.size() == 3 && r.bytes() == 30, "rewind: keeps only the newest snapshots");
    check(r.pop(s) && s[0] == 5 && r.pop(s) && s[0] == 4 && r.pop(s) && s[0] == 3 && !r.pop(s) && r.bytes() == 0,
          "rewind: pops newest first, then runs out");
    r.setLimits(100, 25);
    for (uint8_t i = 1; i <= 5; ++i) r.push(std::vector<uint8_t>(10, i));
    check(r.size() == 2 && r.bytes() == 20, "rewind: the byte limit drops the oldest too");
    r.setLimits(0, 1000);
    r.push(std::vector<uint8_t>(10, 9));
    check(r.empty(), "rewind: no frames means off");
}

void testFramePacer() {
    auto total = [](double speed, int ticks, int* most = nullptr) {
        FramePacer p;
        int sum = 0, top = 0;
        for (int i = 0; i < ticks; ++i) {
            const int n = p.framesForTick(speed);
            sum += n;
            top = std::max(top, n);
        }
        if (most) *most = top;
        return sum;
    };
    int most = 0;
    check(total(1.0, 60, &most) == 60 && most == 1, "speed: normal runs one frame per tick");
    check(total(4.0, 60, &most) == 240 && most == 4, "speed: fast forward x4");
    check(total(0.5, 60, &most) == 30 && most == 1, "speed: slow motion at 50% runs every other tick");
    check(total(0.1, 100) == 10, "speed: 10% adds up without losing frames to rounding");
    check(total(2.5, 4) == 10, "speed: fractions carry over between ticks");
}

void testRewindPlayback() {
    // Rewinding n frames and playing them again with the same pads gives the same
    // picture as the first time: the snapshots are full states.
    MegaDrive md;
    md.loadRomData(testrom::build());
    md.setSampleRate(44100);
    RewindBuffer r;
    r.setLimits(600, size_t{64} << 20);
    std::vector<uint32_t> first;
    auto pads = [](int i) {
        PadState p;
        p.right = (i / 5) % 2 == 0;
        p.a = i % 3 == 0;
        return p;
    };
    double slowest = 0;
    for (int i = 0; i < 120; ++i) {
        std::vector<uint8_t> s;
        const auto t0 = std::chrono::steady_clock::now();
        md.saveState(s, true);
        slowest = std::max(slowest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        r.push(std::move(s));
        md.setPads(pads(i), PadState{});
        md.runFrame();
        md.audio().clearSamples();
        if (i >= 60) first.push_back(frameHash(md.frame()));
    }
    std::printf("     (%zu snapshots, %zu KB, the slowest took %.2f ms)\n", r.size(), r.bytes() / 1024, slowest);
    std::vector<uint8_t> s;
    std::string error;
    bool ok = true;
    for (int i = 0; i < 60; ++i) ok &= r.pop(s);
    ok &= md.loadState(s, &error);
    std::vector<uint32_t> again;
    for (int i = 60; i < 120; ++i) {
        md.setPads(pads(i), PadState{});
        md.runFrame();
        md.audio().clearSamples();
        again.push_back(frameHash(md.frame()));
    }
    check(ok && r.size() == 60, "rewind: went back 60 frames");
    check(again == first, "rewind: replaying from there gives the same frames");

    std::vector<uint8_t> quick;
    md.saveState(quick, true);
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    check(!MegaDrive::stateThumbnail(quick, pixels, &w, &h), "rewind: quick snapshots carry no thumbnail");
}

void testRunAhead() {
    // Run-ahead shows the next frame now and leaves no trace: the console goes on as if
    // it had never run ahead, with the same sound.
    auto pads = [](int i) {
        PadState p;
        p.right = (i / 10) % 2 == 0;
        p.a = (i / 7) % 2 == 0;
        return p;
    };
    MegaDrive plain, ahead;
    // The pad test ROM draws the buttons held, so the picture follows the pads.
    plain.loadRomData(testrom::buildPadTest());
    ahead.loadRomData(testrom::buildPadTest());
    plain.setSampleRate(44100);
    ahead.setSampleRate(44100);
    std::vector<uint32_t> plainFrames, aheadFrames;
    std::vector<int16_t> plainSound, aheadSound;
    bool back = true;
    double aheadMs = 0;
    for (int i = 0; i < 120; ++i) {
        plain.setPads(pads(i), PadState{});
        plain.runFrame();
        plainFrames.push_back(frameHash(plain.frame()));
        plainSound.insert(plainSound.end(), plain.audio().samples().begin(), plain.audio().samples().end());
        plain.audio().clearSamples();

        ahead.setPads(pads(i), PadState{});
        ahead.setDrawing(false);
        ahead.runFrame();
        ahead.setDrawing(true);
        aheadSound.insert(aheadSound.end(), ahead.audio().samples().begin(), ahead.audio().samples().end());
        ahead.audio().clearSamples();
        const auto t0 = std::chrono::steady_clock::now();
        back &= ahead.runAhead(1);
        aheadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        aheadFrames.push_back(frameHash(ahead.frame()));
    }
    // Frame i shown with run-ahead is frame i + 1, wherever the pads did not change.
    bool early = true;
    int compared = 0;
    for (int i = 0; i + 1 < 120; ++i) {
        if (pads(i).right != pads(i + 1).right || pads(i).a != pads(i + 1).a) continue;
        early &= aheadFrames[static_cast<size_t>(i)] == plainFrames[static_cast<size_t>(i + 1)];
        ++compared;
    }
    std::printf("     (a frame of run-ahead took %.2f ms on average)\n", aheadMs / 120);
    std::vector<uint8_t> a, b;
    plain.saveState(a, true);
    ahead.saveState(b, true);
    check(back, "run-ahead: the console went back every frame");
    const std::set<uint32_t> pictures(plainFrames.begin(), plainFrames.end());
    check(early && compared > 90 && pictures.size() > 1, "run-ahead: the picture is the next frame's");
    check(a == b && plain.frameCount() == ahead.frameCount(), "run-ahead: the console is where it would have been");
    check(aheadSound == plainSound, "run-ahead: the sound is the same");
}

void testProfiles() {
    RenderOptions r;
    r.pseudo3d = true;
    r.style = Style3D::Layers;
    r.depth = 0.8f;
    r.tilt = 40;
    r.yaw = -30;
    r.zoom = 1.5f;
    r.extrusion = 0.2f;
    r.thickness = 0.7f;
    r.shadows = false;
    r.highBInFront = true;
    r.flatB = true;
    r.tiltCamera = false;
    r.lighting = false;
    r.liveCamera = true;
    r.smooth = true;  // not part of a profile
    const std::string text = formatProfile(r, "Sonic.md");
    RenderOptions back;
    check(parseProfile(text, back) && same3D(back, r), "profile: written and read back the same");
    check(!back.smooth, "profile: only carries the 3D options");

    RenderOptions partial;
    const RenderOptions defaults;
    check(parseProfile("[perfil_3d]\ninclinacion_3d = 500\nprofundidad_3d = 30\n", partial) && partial.tilt == kMaxTilt &&
              std::fabs(partial.depth - 0.3f) < 1e-6f && partial.yaw == defaults.yaw,
          "profile: missing keys keep their value, out of range ones are clamped");
    RenderOptions untouched;
    check(!parseProfile("[video]\ninclinacion_3d = 10\n", untouched) && untouched.tilt == defaults.tilt,
          "profile: a file without [perfil_3d] is not a profile");

    RenderOptions target;
    target.smooth = true;
    copy3D(r, target);
    check(same3D(target, r) && target.smooth, "profile: copy3D leaves the other options alone");
}

void testLiveCamera() {
    auto frame = std::make_unique<VideoFrameMd>();
    Planes planes;
    planes.width = 320;
    planes.height = 224;
    for (auto& p : planes.layer) p.assign(320 * 224, 0);
    // A sprite on the right of the screen, nothing scrolling.
    for (int y = 100; y < 120; ++y)
        for (int x = 280; x < 300; ++x) planes[PlaneSprites][static_cast<size_t>(y) * 320 + x] = 0xFFFFFFFFu;
    LiveCamera cam;
    for (int i = 0; i < 300; ++i) {
        frame->frameNumber = static_cast<uint64_t>(i + 1);
        cam.update(*frame, planes);
    }
    check(cam.yaw() > 1.0f && cam.yaw() <= LiveCamera::kSwayYaw && std::fabs(cam.tilt()) < 1.0f,
          "live camera: leans towards the sprites, within its limit");

    // No sprites, the level scrolling left (the player running right), wrapping round.
    for (auto& p : planes.layer) p.assign(320 * 224, 0);
    LiveCamera run;
    int scroll = 5;
    for (int i = 0; i < 300; ++i) {
        frame->frameNumber = static_cast<uint64_t>(i + 1);
        for (auto& l : frame->lines) l.hscrollA = static_cast<int16_t>(scroll);
        scroll = (scroll - 4 + 1024) % 1024;
        run.update(*frame, planes);
    }
    check(run.yaw() > 1.0f && run.yaw() <= LiveCamera::kSwayYaw, "live camera: leans with the scroll");
    const float before = run.yaw();
    for (int i = 0; i < 10; ++i) run.update(*frame, planes);  // the same frame again: only eases
    check(std::fabs(run.yaw() - before) < 0.5f, "live camera: a repeated frame does not jolt it");
}

void testCheats() {
    CheatPatch p;
    std::string error;
    // Every bit of address and value goes to its own place: encode, decode, compare.
    bool roundTrip = true;
    std::mt19937 rng(7);
    for (int i = 0; i < 2000; ++i) {
        const uint32_t address = rng() & 0x3FFFFE;
        const uint16_t value = static_cast<uint16_t>(rng());
        const std::string code = encodeGameGenie(address, value);
        roundTrip &= decodeCheat(code, &p, &error) && p.address == address && p.value == value && !p.byte;
    }
    check(roundTrip, "cheats: Game Genie codes decode back to their address and value");
    check(encodeGameGenie(0, 0) == "AAAA-AAAA", "cheats: Game Genie all zeros is AAAA-AAAA");
    check(decodeCheat("aaaa aaaa", &p, &error) && p.address == 0 && p.value == 0, "cheats: lower case and spaces accepted");
    check(!decodeCheat("ABCI-AAAA", &p, &error), "cheats: I is not a Game Genie letter");
    check(!decodeCheat("ABC-AAAA", &p, &error), "cheats: Game Genie needs 8 letters");
    check(decodeCheat("FF0A12:0009", &p, &error) && p.address == 0xFF0A12 && p.value == 9 && !p.byte,
          "cheats: Action Replay word");
    check(decodeCheat("FFB3D5:63", &p, &error) && p.address == 0xFFB3D5 && p.value == 0x63 && p.byte,
          "cheats: Action Replay byte");
    check(decodeCheat("FF0A120009", &p, &error) && p.address == 0xFF0A12 && p.value == 9,
          "cheats: Action Replay without separator");
    check(!decodeCheat("FF0A13:1009", &p, &error), "cheats: word write to an odd address refused");
    check(decodeCheat("FF0A13:0009", &p, &error) && p.address == 0xFF0A13 && p.value == 9 && p.byte,
          "cheats: an odd address with a byte-sized value writes the byte");
    check(!decodeCheat("A10000:0001", &p, &error), "cheats: address outside ROM and RAM refused");

    MegaDrive md;
    md.loadRomData(testrom::build());
    const uint16_t original = md.peek16(0x200);
    std::vector<CheatPatch> list(2);
    decodeCheat(encodeGameGenie(0x200, 0x1234), &list[0], &error);
    decodeCheat("FFF000:BEEF", &list[1], &error);
    md.setCheats(list);
    check(md.peek16(0x200) == 0x1234, "cheats: Game Genie patches the ROM");
    md.runFrame();
    check(md.peek16(0xFFF000) == 0xBEEF, "cheats: Action Replay keeps RAM at its value");
    md.setCheats({});
    check(md.peek16(0x200) == original, "cheats: turning them off restores the ROM");

    std::vector<Cheat> cheats = {{"ABCD-EFGH", "Vidas infinitas", true}, {"FF0A12:0009", "", false}};
    const std::vector<Cheat> back = parseCheats(formatCheats(cheats));
    check(back.size() == 2 && back[0].code == "ABCD-EFGH" && back[0].name == "Vidas infinitas" && back[0].enabled &&
              back[1].code == "FF0A12:0009" && back[1].name.empty() && !back[1].enabled,
          "cheats: cheat file round trip");

    std::vector<CheatPatch> many;
    check(decodeCheatCodes("ajxt-ca80 + FF0A12:0009", &many, &error) && many.size() == 2 && many[1].address == 0xFF0A12,
          "cheats: codes joined with + give all their writes");
    check(!decodeCheatCodes("AJXT-CA80+FF0A12:XX", &many, &error) && many.empty(), "cheats: one bad part refuses the whole code");
    check(!decodeCheatCodes("+", &many, &error), "cheats: nothing between the + is no code");
    const std::string cht = "cheats = 3\n\ncheat0_desc = \"Infinite lives\"\ncheat0_code = \"HADT-BEW6\"\n"
                            "cheat0_enable = false\n\ncheat1_desc = \"Rumble\"\ncheat1_code = \"\"\ncheat1_handler = 1\n"
                            "cheat2_desc = \"Infinite Energy\"\r\ncheat2_code = \"aj0t-ca9y+FFA3C5:0006\"\r\ncheat2_enable = true\r\n";
    const std::vector<Cheat> fromCht = parseChtFile(cht);
    check(fromCht.size() == 2 && fromCht[0].name == "Infinite lives" && fromCht[0].code == "HADT-BEW6" && !fromCht[0].enabled &&
              fromCht[1].code == "AJ0T-CA9Y+FFA3C5:0006" && !fromCht[1].enabled,
          "cheats: RetroArch .cht lists, without the entries that have no code");
    const std::vector<Cheat> kept = parseCheats(formatCheats(fromCht));
    check(kept.size() == 2 && kept[1].code == fromCht[1].code && kept[1].name == "Infinite Energy",
          "cheats: a joined code survives the cheat file");
}

// The detail panel's data: libretro-database's metadat files and Wikipedia's answers.
void testGameInfo() {
    const std::string genres = "clrmamepro (\n\tname \"Sega - Mega Drive - Genesis\"\n)\n\n"
                               "game (\n\tcomment \"Sonic The Hedgehog 2 (World)\"\n\tgenre \"Platform\"\n"
                               "\trom ( crc 24AB4C3A )\n)\n\n"
                               "game (\n\tcomment \"Sonic The Hedgehog 2 (World) (Rev A)\"\n\tgenre \"Platform\"\n)\n\n"
                               "game (\n\tcomment \"Columns (World)\"\n\trom ( crc 03163D7A )\n)\n";
    const auto entries = parseMetadat(genres, "genre");
    check(entries.size() == 2 && entries[0].first == "Sonic The Hedgehog 2 (World)" && entries[0].second == "Platform",
          "game info: metadat entries with the field, the others left out");
    const std::string users = "game (\n\tcomment \"Streets of Rage 2 (USA)\"\n\tusers 2\n\trom ( crc E01FA526 )\n)\n";
    check(parseMetadat(users, "users").size() == 1 && parseMetadat(users, "users")[0].second == "2",
          "game info: unquoted values");
    DetailsTable table;
    table.add("genre", genres);
    table.add("maxusers", users);
    table.add("developer", "game (\n\tcomment \"Sonic The Hedgehog 2 (World)\"\n\tdeveloper \"SEGA\"\n)\n");
    const GameDetails* d = table.find("Sonic The Hedgehog 2 (World)");
    check(d && d->genre == "Platform" && d->developer == "SEGA", "game info: the fields of one game together");
    d = table.find("Sonic The Hedgehog 2 (Europe) [!]");
    check(d && d->developer == "SEGA", "game info: another region of the same title");
    d = table.find("streets of rage 2 (usa)");
    check(d && d->players == "2", "game info: names without case");
    check(!table.find("Ecco the Dolphin (Europe)"), "game info: a game it does not know");

    const std::string answer =
        "{\"batchcomplete\":true,\"query\":{\"pages\":[{\"pageid\":2,\"ns\":0,\"title\":\"Sonic the Hedgehog\","
        "\"index\":2,\"extract\":\"Otro.\"},{\"pageid\":1,\"ns\":0,\"title\":\"Sonic the Hedgehog 2\",\"index\":1,"
        "\"extract\":\"Sonic the Hedgehog 2 es un videojuego de plataformas \\u00e9\\n\\\"Sega\\\".\"}]}}";
    const auto pages = parseWikiSearch(answer);
    check(pages.size() == 2 && pages[0].title == "Sonic the Hedgehog 2" && pages[0].index == 1,
          "game info: Wikipedia's pages in search order");
    check(!pages.empty() && pages[0].extract == "Sonic the Hedgehog 2 es un videojuego de plataformas \xc3\xa9\n\"Sega\".",
          "game info: Wikipedia's escapes");
    check(wikiTitleDistance("Sonic The Hedgehog 2", "Sonic the Hedgehog 2") == 0, "game info: the same title");
    check(wikiTitleDistance("Sonic The Hedgehog 2", "Sonic the Hedgehog") < 0, "game info: not the first game for the second");
    check(wikiTitleDistance("Golden Axe II", "Golden Axe 2 (videojuego)") == 0, "game info: roman numerals and notes");
    check(wikiTitleDistance("Phantasy Star IV", "Phantasy Star IV: The End of the Millennium") > 0,
          "game info: a longer title of the same game");
    check(wikiTitleDistance("Sonic 3D Blast", "Sonic") < 0, "game info: too little in common");
    check(wikiTitleDistance("Columns", "Ecco the Dolphin") < 0, "game info: nothing in common");
}

void testLibraryNames() {
    check(gameTitle("Lion King, The (World)") == "The Lion King", "library: trailing ', The' moves to the front");
    check(gameTitle("Adventures of Batman & Robin, The (USA)") == "The Adventures of Batman & Robin",
          "library: article with an ampersand in the name");
    check(gameTitle("Sonic The Hedgehog 2 (World) (Rev B)") == "Sonic The Hedgehog 2", "library: tags dropped");
    check(gameTitle("Golden Axe III") == "Golden Axe III", "library: name without tags kept");
    check(gameTags("Sonic The Hedgehog 2 (World) (Rev B)") == "World · Rev B", "library: tags listed");
    const auto names = coverNames("Comix Zone (USA) (Sega Smash Pack)");
    check(names.size() > 2 && names[0] == "Comix Zone (USA) (Sega Smash Pack)" && names[1] == "Comix Zone (USA)",
          "library: cover names drop trailing tags one by one");
    check(coverNames("Sonic & Knuckles (World)")[0] == "Sonic _ Knuckles (World)", "library: '&' becomes '_' like libretro");
    const auto bare = coverNames("Golden Axe III");
    check(!bare.empty() && bare[0] == "Golden Axe III (USA)", "library: a bare name tries the usual regions");
    check(urlEncode("Columns (USA, Europe)") == "Columns%20%28USA%2C%20Europe%29", "library: URL encoding");
    check(searchKey("Dr. Robotnik's  Mean-Bean") == "dr robotnik s mean bean", "library: search key");

    const std::string html = "<a href=\"../\">../</a>\n<a href=\"Sonic%20The%20Hedgehog%202%20%28World%29.png\">Sonic...</a>\n"
                             "<a href=\"Ecco%20the%20Dolphin%20%28USA%29.png\">Ecco</a>\n<a href=\"Tools.txt\">x</a>";
    const auto fromHtml = parseCoverList(html);
    check(fromHtml.size() == 2 && fromHtml[0] == "Ecco the Dolphin (USA)" && fromHtml[1] == "Sonic The Hedgehog 2 (World)",
          "library: cover list from libretro's index");
    const std::string json = "{\"tree\":[{\"path\": \"Pok\\u00e9mon (Japan).png\",\"type\":\"blob\"},"
                             "{\"path\":\"Golden Axe II (World).png\"}]}";
    const auto fromJson = parseCoverList(json);
    check(fromJson.size() == 2 && fromJson[0] == "Golden Axe II (World)" && fromJson[1] == "Pok\xc3\xa9mon (Japan)",
          "library: cover list from GitHub's tree");
    const std::vector<std::string> set = {"Golden Axe (World)", "Golden Axe II (World)", "Golden Axe III (Japan)",
                                          "Sonic The Hedgehog (USA, Europe)", "Sonic The Hedgehog 2 (World)",
                                          "Sonic The Hedgehog 3 (USA)", "Streets of Rage 2 (USA)", "Lion King, The (World)"};
    auto similar = similarCoverNames(set, "Golden Axe 2", 3);
    check(!similar.empty() && similar[0] == "Golden Axe II (World)", "library: '2' finds the 'II' cover first");
    similar = similarCoverNames(set, "sonic hedgehod 3", 2);
    check(!similar.empty() && similar[0] == "Sonic The Hedgehog 3 (USA)", "library: a typo still finds the cover");
    similar = similarCoverNames(set, "The Lion King (Europe)", 5);
    check(!similar.empty() && similar[0] == "Lion King, The (World)", "library: ', The' and other regions");
    check(similarCoverNames(set, "Columns", 5).empty(), "library: nothing alike, no suggestions");

    const std::string tree = "{\"tree\":[{\"path\":\"Sonic The Hedgehog (USA, Europe).cht\"},"
                             "{\"path\":\"Sonic The Hedgehog 2 (World) (Rev A).cht\"},{\"path\":\"Sonic The Hedgehog 2 (World).cht\"},"
                             "{\"path\":\"Sonic The Hedgehog 2 (World) (Action Replay).cht\"},{\"path\":\"cover.png\"}]}";
    const auto chts = parseFileList(tree, ".cht");
    check(chts.size() == 4 && std::is_sorted(chts.begin(), chts.end()), "library: cheat lists from GitHub's tree");
    auto lists = cheatFileNames(chts, "Sonic The Hedgehog 2 (World)", 5);
    check(lists.size() == 4 && lists[0] == "Sonic The Hedgehog 2 (World)" &&
              lists[1] == "Sonic The Hedgehog 2 (World) (Action Replay)",
          "library: the cheat list named like the ROM first, then its Action Replay one, then similar ones");
    lists = cheatFileNames(chts, "Sonic The Hedgehog 2 (Europe)", 5);
    check(!lists.empty() && lists[0] == "Sonic The Hedgehog 2 (World)", "library: another region's cheat list");
    check(cheatFileNames(chts, "Columns (World)", 5).empty(), "library: no cheat lists for an unknown game");
}

void testSystems() {
    namespace fs = std::filesystem;
    std::vector<uint8_t> md = testrom::build();
    check(detectSystemFromData(md) == ConsoleSystem::MegaDrive, "systems: a Mega Drive ROM");
    std::vector<uint8_t> mars = md;
    std::copy_n("SEGA 32X        ", 16, mars.begin() + 0x100);
    check(detectSystemFromData(mars) == ConsoleSystem::Mega32X, "systems: a 32X ROM dumped as .bin");
    std::vector<uint8_t> disc(0x930, 0);
    std::copy_n("SEGADISCSYSTEM", 14, disc.begin() + 0x10);
    check(detectSystemFromData(disc) == ConsoleSystem::MegaCD, "systems: a raw disc track");
    std::copy_n("SEGADISCSYSTEM", 14, disc.begin());
    check(detectSystemFromData(disc) == ConsoleSystem::MegaCD, "systems: an ISO track");

    const std::string cue = "REM test\nFILE \"Sonic CD (USA) (Track 01).bin\" BINARY\n  TRACK 01 MODE1/2352\n"
                            "FILE track02.bin BINARY\n";
    const auto files = cueFilesFromText(cue, "dir/");
    check(files.size() == 2 && files[0] == "dir/Sonic CD (USA) (Track 01).bin" && files[1] == "dir/track02.bin",
          "systems: files of a .cue sheet");

    // A folder with one game of each console, a disc's tracks and a BIOS.
    const fs::path dir = fs::temp_directory_path() / "plumsg_systems_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "cd");
    auto write = [](const fs::path& p, const std::vector<uint8_t>& d) {
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    };
    write(dir / "Game (USA).md", md);
    write(dir / "Mars.bin", mars);
    write(dir / "Other.32x", mars);
    write(dir / "cd" / "Disc (Track 01).bin", disc);
    std::ofstream(dir / "cd" / "Disc.cue") << "FILE \"Disc (Track 01).bin\" BINARY\n";
    check(isGameFile((dir / "cd" / "Disc.cue").string()) && !isGameFile("notes.txt"), "systems: game file extensions");
    check(detectSystem((dir / "Game (USA).md").string()) == ConsoleSystem::MegaDrive &&
              detectSystem((dir / "Mars.bin").string()) == ConsoleSystem::Mega32X &&
              detectSystem((dir / "Other.32x").string()) == ConsoleSystem::Mega32X &&
              detectSystem((dir / "cd" / "Disc.cue").string()) == ConsoleSystem::MegaCD,
          "systems: detected from the files");
    const auto tracks = cueFiles((dir / "cd" / "Disc.cue").string());
    check(tracks.size() == 1 && fs::exists(fs::u8path(tracks[0])), "systems: .cue tracks resolved next to the sheet");

    std::vector<uint8_t> bios(0x20000, 0);
    std::copy_n("SEGA MEGA DRIVE ", 16, bios.begin() + 0x100);
    std::copy_n("MEGA-CD BOOT ROM", 16, bios.begin() + 0x150);
    bios[0x1F0] = 'U';
    const BiosInfo info = identifyCdBios(bios);
    check(info.valid && info.region == Region::UsaNtsc && info.name == "MEGA-CD BOOT ROM", "systems: a Mega CD BIOS");
    check(!identifyCdBios(md).valid, "systems: a cartridge is not a BIOS");
    write(dir / "cd" / "bios_CD_U.bin", bios);
    bios[0x1F0] = ' ';
    write(dir / "bios_CD_E.bin", bios);
    const auto found = findCdBios({dir.u8string()});
    check(found[0].empty() && fs::path(found[1]).filename() == "bios_CD_U.bin" &&
              fs::path(found[2]).filename() == "bios_CD_E.bin",
          "systems: BIOS found by header region or file name");
    fs::remove_all(dir);
}

// A network between two netplay sessions, in memory: each packet arrives `delay`
// milliseconds later (plus some jitter, so some overtake others), and some are lost
// or arrive twice.
struct SimLink {
    struct Packet {
        uint32_t at;
        std::vector<uint8_t> data;
    };
    std::vector<Packet> toA, toB;
    std::mt19937 rng{1234};
    uint32_t now = 0;
    uint32_t delay = 40;
    int lossPercent = 10;

    void post(std::vector<Packet>& q, const uint8_t* d, size_t n) {
        if (static_cast<int>(rng() % 100) < lossPercent) return;
        const int copies = rng() % 20 == 0 ? 2 : 1;
        for (int i = 0; i < copies; ++i)
            q.push_back({now + delay + static_cast<uint32_t>(rng() % 30), std::vector<uint8_t>(d, d + n)});
    }
    static bool take(std::vector<Packet>& q, uint32_t now, std::vector<uint8_t>& out) {
        for (size_t i = 0; i < q.size(); ++i)
            if (q[i].at <= now) {
                out = std::move(q[i].data);
                q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        return false;
    }
};

struct SimEnd : NetTransport {
    SimLink& link;
    bool isA;
    SimEnd(SimLink& l, bool a) : link(l), isA(a) {}
    void send(const uint8_t* d, size_t n) override { link.post(isA ? link.toB : link.toA, d, n); }
    bool receive(std::vector<uint8_t>& p) override { return SimLink::take(isA ? link.toA : link.toB, link.now, p); }
};

// What each player presses on their n-th frame: a new random pad every few frames.
PadState scriptedPad(int player, uint32_t n) {
    std::mt19937 rng(static_cast<uint32_t>(player * 7919 + n / 5));
    return unpackPad(static_cast<uint16_t>(rng() & 0xFFF));
}

void testNetplay() {
    check(packPad(unpackPad(0xA5C)) == 0xA5C && packPad(PadState{}) == 0, "netplay: los mandos caben en 12 bits");

    constexpr uint32_t kFrames = 400;
    NetplaySession::Config config;
    config.inputDelay = 2;
    config.maxRollback = 8;
    // Without the network: frame f has the pads pressed on frame f - inputDelay.
    std::vector<uint32_t> reference;
    {
        MegaDrive md;
        md.loadRomData(testrom::buildTwoPadTest());
        for (int i = 0; i < 30; ++i) md.runFrame();  // the host played a little alone first
        for (uint32_t f = 0; f < kFrames; ++f) {
            const bool early = f < static_cast<uint32_t>(config.inputDelay);
            const uint32_t n = early ? 0 : f - static_cast<uint32_t>(config.inputDelay);
            md.setPads(early ? PadState{} : scriptedPad(0, n), early ? PadState{} : scriptedPad(1, n));
            md.runFrame();
            reference.push_back(frameHash(md.frame()));
        }
    }

    SimLink link;
    SimEnd hostEnd(link, true), guestEnd(link, false);
    NetplaySession host(hostEnd, NetplaySession::Role::Host, config);
    NetplaySession guest(guestEnd, NetplaySession::Role::Guest, config);
    MegaDrive a, b;
    a.loadRomData(testrom::buildTwoPadTest());
    b.loadRomData(testrom::buildTwoPadTest());
    for (int i = 0; i < 30; ++i) a.runFrame();
    b.setPadType(0, PadType::ThreeButton);  // the host's pad types win
    std::vector<uint32_t> hashA(kFrames), hashB(kFrames);
    host.onFrame = [&](uint32_t f, MegaDrive& md) { if (f < kFrames) hashA[f] = frameHash(md.frame()); };
    guest.onFrame = [&](uint32_t f, MegaDrive& md) { if (f < kFrames) hashB[f] = frameHash(md.frame()); };
    int rollbacks = 0, stalls = 0;
    uint32_t hostTicks = 0, guestTicks = 0;
    for (int step = 0; step < 40000; ++step) {
        link.now = static_cast<uint32_t>(step);  // one millisecond per step
        // The two displays tick at slightly different rates, as two monitors do.
        const bool hostTick = step % 16 == 0, guestTick = step % 17 == 3;
        host.poll(a, link.now);
        guest.poll(b, link.now);
        if (hostTick && host.frame() < kFrames) {
            if (!host.advance(a, scriptedPad(0, host.frame()))) ++stalls;
            rollbacks += host.lastRollback() > 0;
            ++hostTicks;
        }
        if (guestTick && guest.frame() < kFrames) {
            if (!guest.advance(b, scriptedPad(1, guest.frame()))) ++stalls;
            rollbacks += guest.lastRollback() > 0;
            ++guestTicks;
        }
        if (host.frame() >= kFrames) host.catchUp(a);
        if (guest.frame() >= kFrames) guest.catchUp(b);
        if (host.confirmedFrames() >= kFrames && guest.confirmedFrames() >= kFrames) break;
    }
    check(host.phase() == NetplaySession::Phase::Playing && guest.phase() == NetplaySession::Phase::Playing,
          "netplay: el invitado se conecta y los dos juegan");
    check(host.confirmedFrames() >= kFrames && guest.confirmedFrames() >= kFrames,
          "netplay: los dos confirman todos los fotogramas pese a perder paquetes");
    check(rollbacks > 0, "netplay: hay vueltas atrás cuando el mando del otro llega tarde");
    check(hashA == hashB, "netplay: las dos consolas ven lo mismo en cada fotograma");
    check(hashA == reference, "netplay: igual que jugando los dos en la misma consola");
    check(host.desyncFrame() < 0 && guest.desyncFrame() < 0, "netplay: no se detecta ningún descompás");
    check(host.pingMs() > 60 && host.pingMs() < 140, "netplay: el ping se mide (unos 2 x 40 ms)");
    std::printf("     %d vueltas atrás, %d esperas, ping %d ms\n", rollbacks, stalls, host.pingMs());

    guest.end();
    for (uint32_t t = 0; t < 200; ++t) {
        link.now += 1;
        host.poll(a, link.now);
    }
    check(host.phase() == NetplaySession::Phase::Ended && host.endReason().find("salido") != std::string::npos,
          "netplay: el anfitrión se entera de que el invitado se va");

    // A guest with another game is turned away with a reason.
    SimLink link2;
    link2.lossPercent = 0;
    SimEnd h2(link2, true), g2(link2, false);
    NetplaySession host2(h2, NetplaySession::Role::Host, config);
    NetplaySession guest2(g2, NetplaySession::Role::Guest, config);
    MegaDrive c, d;
    c.loadRomData(testrom::buildTwoPadTest());
    d.loadRomData(testrom::buildPadTest());
    for (int step = 0; step < 2000 && guest2.phase() != NetplaySession::Phase::Ended; ++step) {
        link2.now = static_cast<uint32_t>(step);
        host2.poll(c, link2.now);
        guest2.poll(d, link2.now);
    }
    check(guest2.phase() == NetplaySession::Phase::Ended && guest2.endReason().find("otro juego") != std::string::npos &&
              host2.phase() == NetplaySession::Phase::Waiting,
          "netplay: un invitado con otro juego no entra y el anfitrión sigue esperando");
}

// The Roland sound: register writes become the right MIDI events.
// RetroAchievements: the game's hash, its memory as the achievements address it, and a
// whole session (login, game, an achievement earned) against a made-up server.
void testAchievements() {
    MegaDrive md;
    md.loadRomData(std::vector<uint8_t>(0x200, 0));
    check(Achievements::gameHash(md) == "bf619eac0cdf3f68d496ea9344137e8b", "logros: hash = MD5 de la ROM");
    md.poke(0xFF0010, 0x12);
    md.poke(0xFF0011, 0x34);
    md.poke(0xFFFFFF, 0x56);
    uint8_t bytes[3] = {};
    check(Achievements::readMemory(md, 0x10, bytes, 2) == 2 && bytes[0] == 0x34 && bytes[1] == 0x12,
          "logros: la RAM del 68000 con los bytes de cada palabra cambiados");
    check(Achievements::readMemory(md, 0xFFFE, bytes, 2) == 2 && bytes[0] == 0x56, "logros: el final de la RAM");
    check(Achievements::readMemory(md, 0x20000, bytes, 1) == 0, "logros: nada más allá de la SRAM");

    std::vector<std::string> asked;
    auto server = [&](const std::string& url, const std::string& post, const std::string& agent, std::string& body) {
        asked.push_back(post);
        check(agent.rfind("plumsg/", 0) == 0 && agent.find("rcheevos/") != std::string::npos,
              "logros: el User-Agent dice plumsg y rcheevos");
        (void)url;
        if (post.rfind("r=login2", 0) == 0)
            body = "{\"Success\":true,\"User\":\"Probador\",\"Token\":\"FICHA\",\"Score\":100,\"SoftcoreScore\":0,"
                   "\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}";
        else if (post.rfind("r=achievementsets", 0) == 0)
            body = "{\"Success\":true,\"GameId\":7,\"Title\":\"Juego de prueba\",\"ConsoleId\":1,"
                   "\"ImageIconUrl\":\"http://servidor/juego.png\",\"RichPresenceGameId\":7,\"RichPresencePatch\":\"\","
                   "\"Sets\":[{\"AchievementSetId\":70,\"GameId\":7,\"Title\":null,\"Type\":\"core\","
                   "\"ImageIconUrl\":\"http://servidor/juego.png\",\"Achievements\":["
                   "{\"ID\":701,\"Title\":\"Cinco\",\"Description\":\"Llega a cinco\",\"Flags\":3,\"Points\":10,"
                   "\"MemAddr\":\"0xH0010=5\",\"Author\":\"Alguien\",\"BadgeName\":\"00001\","
                   "\"Created\":1367266583,\"Modified\":1376929305}],\"Leaderboards\":[]}]}";
        else if (post.rfind("r=startsession", 0) == 0)
            body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
        else if (post.rfind("r=awardachievement", 0) == 0)
            body = "{\"Success\":true,\"Score\":110,\"SoftcoreScore\":0,\"AchievementID\":701,\"AchievementsRemaining\":0}";
        else
            body = "{\"Success\":true}";
        return 200;
    };
    Achievements ra(server, false);
    ra.loadGame(md, Achievements::gameHash(md));  // before logging in: it waits for it
    ra.loginWithPassword("Probador", "secreta");
    for (int i = 0; i < 6; ++i) ra.poll();
    check(ra.loggedIn() && ra.token() == "FICHA", "logros: sesión iniciada, con la ficha para la próxima vez");
    check(ra.gameLoaded() && ra.gameTitle() == "Juego de prueba" && ra.achievementCount() == 1,
          "logros: el juego identificado por su hash, con su logro");
    bool gameNotice = false;
    for (const auto& n : ra.takeNotices()) gameNotice |= n.kind == Achievements::Notice::Kind::GameStarted;
    check(gameNotice, "logros: aviso de juego con logros");

    md.poke(0xFF0011, 0);
    ra.doFrame();
    ra.doFrame();
    check(ra.takeNotices().empty(), "logros: nada mientras no se cumple");
    md.poke(0xFF0011, 5);  // address 0x10 for the achievements
    ra.doFrame();
    const auto notices = ra.takeNotices();
    check(notices.size() == 1 && notices[0].kind == Achievements::Notice::Kind::Unlocked && notices[0].title == "Cinco" &&
              notices[0].points == 10,
          "logros: logro conseguido al cumplirse");
    ra.poll();
    check(!asked.empty() && asked.back().rfind("r=awardachievement", 0) == 0 && asked.back().find("a=701") != std::string::npos,
          "logros: el logro se envía al servidor");
    check(ra.unlockedCount() == 1, "logros: cuenta como conseguido");

    // Savestates keep the progress; an empty one starts over.
    const std::vector<uint8_t> progress = ra.saveProgress();
    check(!progress.empty(), "logros: el progreso cabe en el estado");
    ra.unloadGame();
    check(!ra.gameLoaded(), "logros: juego cerrado");
}

// The Partitura's figures: the tempo and bars guessed, and notes written as figures.
void testNotation() {
    using namespace notation;
    // Quarters at 150 BPM (0.4 s), from 1.3 s, with eighths now and then.
    std::vector<double> onsets;
    for (int i = 0; i < 32; ++i) {
        onsets.push_back(1.3 + 0.4 * i);
        if (i % 4 == 3) onsets.push_back(1.3 + 0.4 * i + 0.2);
    }
    const Meter m = guessMeter(onsets);
    check(m.bpm() == 150, "Notación: adivina el tempo de negras a 150 BPM");
    check(std::fabs(std::remainder(m.origin - 1.3, m.quarter)) < 0.02, "Notación: los compases empiezan en una nota");
    // Notes a second apart are two beats at 120 BPM: the tempo is halved into 70-150.
    check(guessMeter({0, 1, 2, 3, 4, 5}).bpm() == 120, "Notación: el tempo cae entre 70 y 150 BPM");

    Meter q;  // 120 BPM from 0: a sixteenth is 0.125 s
    q.quarter = 0.5;
    q.origin = 0;
    check(split(0, 16) == std::vector<int>{16} && split(2, 6) == std::vector<int>{6} && split(1, 3) == std::vector<int>{1, 2} &&
              split(4, 12) == std::vector<int>{12} && split(0, 12, true) == std::vector<int>{8, 4},
          "Notación: las duraciones se escriben con las figuras que caben donde empiezan");
    // A quarter (let go early), a dotted quarter and an eighth, then a rest of a half
    // (as two quarters: it starts on the fourth beat and goes over the bar line).
    const std::vector<Played> line = {{0.0, 0.4, 60, 1}, {0.5, 1.2, 62, 2}, {1.25, 1.45, 64, 3}};
    const std::vector<Figure> f = transcribe(line, q, 0, 20);
    check(f.size() == 5 && f[0].length == 4 && !f[0].rest && f[1].length == 6 && f[2].length == 2 && f[2].notes == std::vector<int>{64} &&
              f[3].rest && f[3].length == 4 && f[4].rest && f[4].at == 16 && f[4].length == 4,
          "Notación: negra, negra con puntillo, corchea y silencios de negra a cada lado de la barra");
    // A note over the bar line is tied; notes starting together are a chord.
    const std::vector<Figure> t = transcribe({{1.5, 2.5, 67, 1}, {1.5, 2.5, 60, 2}}, q, 12, 20);
    check(t.size() == 2 && t[0].tied && t[0].length == 4 && t[1].at == 16 && t[1].length == 4 && !t[1].tied &&
              t[0].notes == std::vector<int>{60, 67},
          "Notación: una nota que cruza la barra se liga, y las que empiezan juntas son un acorde");
    // Written from any bar line, a passage comes out the same: the score does not change
    // as it scrolls.
    {
        std::vector<Played> tune;
        const double starts[] = {0.1, 0.6, 0.85, 1.6, 2.3, 2.45, 3.1, 4.4, 5.0, 5.2, 6.9};
        for (int i = 0; i < 11; ++i) tune.push_back({starts[i], starts[i] + 0.2, 60 + i, i});
        auto key = [](const std::vector<Figure>& figs, long long from) {
            std::vector<std::tuple<long long, int, bool, bool>> out;
            for (const Figure& x : figs)
                if (x.at >= from) out.emplace_back(x.at, x.length, x.rest, x.tied);
            return out;
        };
        check(key(transcribe(tune, q, 0, 64), 32) == key(transcribe(tune, q, 16, 64), 32) &&
                  key(transcribe(tune, q, 0, 64), 32) == key(transcribe(tune, q, 32, 64), 32),
              "Notación: las figuras no cambian según desde qué compás se escriban");
    }
}

void testRoland() {
    Ym2612 ym;
    ym.setMasterClock(MegaDrive::kMasterNtsc);
    Psg psg;
    RolandSound roland;
    std::vector<std::string> log;
    roland.setLog(&log);
    auto fm = [&](int port, int reg, int value) {
        ym.write(port * 2, static_cast<uint8_t>(reg));
        roland.fmWrite(port * 2, static_cast<uint8_t>(reg), ym, 0);
        ym.write(port * 2 + 1, static_cast<uint8_t>(value));
        roland.fmWrite(port * 2 + 1, static_cast<uint8_t>(value), ym, 0);
    };
    auto has = [&](const std::string& line) { return std::find(log.begin(), log.end(), line) != log.end(); };

    check(std::fabs(RolandSound::fmNote(1083, 4, MegaDrive::kMasterNtsc / 7) - 69.0) < 0.05,
          "Roland: F-number 1083 bloque 4 es el La 440");
    check(std::fabs(RolandSound::psgNote(254, Psg::kClockNtsc) - 69.0) < 0.05, "Roland: periodo PSG 254 es el La 440");

    fm(0, 0xB0, 0x04);  // algorithm 4: operators 2 and 4 heard
    fm(0, 0xB4, 0xC0);  // both sides
    for (int op : {0x40, 0x44, 0x48, 0x4C}) fm(0, op, 0);
    fm(0, 0xA4, 0x22);  // block 4, F-number 0x269: about 250 Hz, B3
    fm(0, 0xA0, 0x69);
    fm(0, 0x28, 0xF0);
    check(has("0 on 59"), "Roland: key on del canal FM 1 toca la nota 59");
    check(log.size() >= 2 && log[0].rfind("0 program", 0) == 0, "Roland: elige un instrumento antes de la nota");
    log.clear();
    fm(0, 0xA4, 0x22);
    fm(0, 0xA0, 0x7A);  // a little higher: bends, no new note
    check(!log.empty() && log.back().rfind("0 bend", 0) == 0 && !has("0 on 59"), "Roland: subir la frecuencia dobla la nota");
    fm(0, 0x28, 0x00);
    check(has("0 off 59"), "Roland: key off apaga la nota");
    log.clear();
    fm(0, 0x2B, 0x80);  // channel 6 plays samples
    fm(0, 0x28, 0xF6);
    check(log.empty(), "Roland: el canal 6 con el DAC no da notas");

    psg.write(0x8E);  // channel 0 period 0x0FE
    psg.write(0x0F);
    roland.psgWrite(psg, 0);
    psg.write(0x90);  // full volume
    roland.psgWrite(psg, 0);
    check(has("6 on 69"), "Roland: el canal 1 del PSG toca un La");
    psg.write(0x9F);
    roland.psgWrite(psg, 0);
    check(has("6 off 69"), "Roland: silenciarlo apaga la nota");

    std::array<uint8_t, 28> ops{};
    check(RolandSound::programFor(ops, 0x07, 40) == 39, "Roland: una nota grave sostenida y sin modular es un bajo sintetizado");
    check(RolandSound::programFor(ops, 0x07, 72) == 16, "Roland: el algoritmo 7 es un órgano");
    {
        // Algorithm 0 (operator 4 the carrier), a carrier that dies away fast.
        std::array<uint8_t, 28> p{};
        for (int op = 0; op < 4; ++op) p[static_cast<size_t>(op * 7 + 2)] = 31;  // fastest attack
        p[3 * 7 + 3] = 24;    // D1R
        p[3 * 7 + 5] = 0xF0;  // sustain level: silence
        for (int op = 0; op < 3; ++op) p[static_cast<size_t>(op * 7 + 1)] = 80;  // modulators hardly heard
        check(RolandSound::programFor(p, 0x00, 72) == 13, "Roland: un sonido seco y suave es un xilófono");
        RolandSound::Heard kick;
        for (int i = 0; i < 6; ++i) kick.add(40);
        kick.held = 6;
        kick.heldSeconds = 0.3;
        check(RolandSound::suggest(p, 0x00, kick).drum == 36, "Roland: una nota grave repetida y corta es un bombo");
        RolandSound::Heard snare = kick;
        check(RolandSound::suggest(p, 0x38, snare).drum == 38, "Roland: con mucho feedback es una caja");
        RolandSound::Heard line;
        for (int n : {40, 43, 45, 47, 40, 43}) line.add(n);
        line.held = 6;
        line.heldSeconds = 0.3;
        check(RolandSound::suggest(p, 0x00, line).program == 33, "Roland: una línea grave que muere es un bajo");
        // A modulator at 7 times the carrier: a bell.
        p[0] = 7;
        p[1] = 20;
        check(RolandSound::programFor(p, 0x00, 88) == 9, "Roland: un modulador lejano y un sonido que muere es un carillón");
        // Held, a modulator at twice the carrier: only odd harmonics.
        std::array<uint8_t, 28> c{};
        for (int op = 0; op < 4; ++op) c[static_cast<size_t>(op * 7 + 2)] = 31;
        for (int op = 0; op < 3; ++op) c[static_cast<size_t>(op * 7 + 1)] = 127;
        c[2 * 7 + 1] = 40;
        c[2 * 7] = 2;
        c[3 * 7] = 1;
        check(RolandSound::programFor(c, 0x00, 67) == 71, "Roland: armónicos impares y suaves es un clarinete");
        check(RolandSound::programFor(c, 0x30, 67) == 80, "Roland: con feedback el clarinete se vuelve onda cuadrada");
        RolandSound::Heard pads;
        for (int i = 0; i < 8; ++i) pads.add(60 + i);
        pads.chords = 6;
        c[2 * 7 + 1] = 0;  // bright: a lead
        c[2 * 7] = 1;
        check(RolandSound::programFor(c, 0x00, 67) == 81, "Roland: una onda brillante sostenida es un lead");
        check(RolandSound::suggest(c, 0x00, pads).program == 90, "Roland: si toca acordes es un pad");
    }
    {
        // Comparing sounds: an FM sine is closer to a sine than to a sawtooth.
        const double rate = 44100, hz = 261.63;
        std::vector<float> sine(static_cast<size_t>(rate / 2)), saw(sine.size());
        for (size_t i = 0; i < sine.size(); ++i) {
            const double t = static_cast<double>(i) * hz / rate;
            sine[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * t));
            saw[i] = static_cast<float>(0.5 * (2 * (t - std::floor(t)) - 1));
        }
        const auto sinePrint = TimbreMatcher::measure(sine, rate, hz), sawPrint = TimbreMatcher::measure(saw, rate, hz);
        std::array<uint8_t, 28> pure{};
        for (int op = 0; op < 4; ++op) {
            pure[static_cast<size_t>(op * 7)] = 1;          // MUL 1
            pure[static_cast<size_t>(op * 7 + 1)] = 127;    // modulators silent
            pure[static_cast<size_t>(op * 7 + 2)] = 31;     // fastest attack
        }
        const auto fm = TimbreMatcher::fmPrint(pure, 0x00, 60);
        check(!fm.silent && TimbreMatcher::distance(fm, sinePrint) < TimbreMatcher::distance(fm, sawPrint),
              "Timbre: un FM sin modular se parece más a un seno que a un diente de sierra");
        std::array<uint8_t, 28> bright = pure;
        bright[2 * 7 + 1] = 10;  // operator 3 modulating hard
        const auto fmBright = TimbreMatcher::fmPrint(bright, 0x00, 60);
        check(TimbreMatcher::distance(fmBright, sawPrint) < TimbreMatcher::distance(fm, sawPrint),
              "Timbre: modulado fuerte se acerca al diente de sierra");
        check(TimbreMatcher::closest(nullptr, pure, 0x00, 60, -1) == -1, "Timbre: sin SoundFont no elige nada");
    }

    // Instruments chosen by hand: a drum, an octave up, the chip's own sound.
    const auto seenFm = roland.seen();
    check(seenFm.size() >= 2, "Roland: anota los instrumentos que suenan (FM y PSG)");
    uint32_t fmPrint = 0;
    for (const auto& [fp, seen] : seenFm)
        if (seen.kind == RolandSound::Kind::Fm) fmPrint = fp;
    check(fmPrint && (seenFm.at(fmPrint).channels & 1) && seenFm.at(fmPrint).rules.program >= 0,
          "Roland: el instrumento FM sabe su canal y su sugerencia");
    fm(0, 0x2B, 0x00);
    RolandSound::Assignments chosen;
    chosen[fmPrint].drum = 38;
    roland.setAssignments(chosen);
    log.clear();
    fm(0, 0x28, 0xF0);
    check(has("9 on 38"), "Roland: un instrumento FM asignado a la caja suena en el canal de batería");
    fm(0, 0x28, 0x00);
    chosen[fmPrint] = RolandSound::Assignment{};
    chosen[fmPrint].program = 0;
    chosen[fmPrint].octave = 1;
    roland.setAssignments(chosen);
    fm(0, 0xA4, 0x22);  // back to B3
    fm(0, 0xA0, 0x69);
    log.clear();
    fm(0, 0x28, 0xF0);
    check(has("0 on 71") && has("0 program 0"), "Roland: con una octava más, el Si3 suena como Si4");
    fm(0, 0x28, 0x00);
    chosen[fmPrint] = RolandSound::Assignment{};
    chosen[fmPrint].original = true;
    roland.setAssignments(chosen);
    log.clear();
    fm(0, 0x28, 0xF0);
    check(log.empty() && (roland.chipChannels() & 1), "Roland: un instrumento original no da notas y deja oír el chip");
    fm(0, 0x28, 0x00);
    roland.setAssignments({});
    roland.setListening(1, {});  // FM 1 muted
    log.clear();
    fm(0, 0x28, 0xF0);
    check(log.empty(), "Roland: un canal en silencio no da notas");
    roland.setListening(0, {});
    fm(0, 0x28, 0x00);

    // The score: the notes at the chip's pitch, whatever they sound as, without drums.
    {
        const RolandSound::Score& score = roland.score();
        const bool b3 = std::any_of(score.begin(), score.end(), [](const RolandSound::ScoreNote& n) { return n.note == 59 && n.voice == 0; });
        const bool drums = std::any_of(score.begin(), score.end(), [](const RolandSound::ScoreNote& n) { return n.note == 38; });
        const bool ended = std::all_of(score.begin(), score.end(), [](const RolandSound::ScoreNote& n) { return n.end > n.start; });
        check(b3 && !drums && ended, "Roland: la partitura escribe el Si3 tal cual (octava, original o en silencio), sin la batería");
        // Only following: no notes to the synth, every channel to the chips but an
        // instrument switched off.
        roland.setFollowOnly(true);
        log.clear();
        fm(0, 0x28, 0xF0);
        check(log.empty() && (roland.chipChannels() & 1) && roland.score().back().end == 0,
              "Roland: solo siguiendo, la nota va a la partitura y suena el chip");
        roland.setListening(0, {fmPrint});
        check(!(roland.chipChannels() & 1), "Roland: un instrumento apagado calla también en el chip");
        roland.setListening(0, {});
        fm(0, 0x28, 0x00);
        roland.setFollowOnly(false);
        roland.clearScore();
        check(roland.score().empty(), "Roland: la partitura se vacía");
    }

    // Sonic's driver sets the release rates to the fastest to cut a note: it is still
    // the same instrument, and what an old file chose under either old fingerprint holds.
    for (int op : {0x80, 0x84, 0x88, 0x8C}) fm(0, op, 0x0F);
    fm(0, 0x28, 0xF0);
    fm(0, 0x28, 0x00);
    int fmSeen = 0;
    for (const auto& [fp, seen] : roland.seen()) fmSeen += seen.kind == RolandSound::Kind::Fm;
    check(fmSeen == 1, "Roland: cambiar la velocidad de release no hace otro instrumento");
    // With release rate 0 the old fingerprint was the same; with 15, another one.
    const std::vector<uint32_t> old = roland.aliases(fmPrint);
    check(old.size() == 1 && old[0] != fmPrint, "Roland: se recuerda la huella de antes del instrumento");
    if (old.size() == 1) {
        chosen.clear();
        chosen[old[0]].drum = 42;
        roland.setAssignments(chosen);
        log.clear();
        fm(0, 0x28, 0xF0);
        check(has("9 on 42") && roland.assignedAs(fmPrint) == old[0], "Roland: una asignación con la huella de antes vale");
        fm(0, 0x28, 0x00);
        roland.setAssignments({});
    }

    // The assignment files.
    RolandSound::Assignments files;
    files[0x1234abcd].program = 81;
    files[0x1234abcd].octave = -1;
    files[0x1234abcd].volume = 80;
    files[0x0badf00d].drum = 42;
    files[0x4E4F4904].original = true;
    const RolandSound::Assignments back = RolandSound::parseAssignments(RolandSound::formatAssignments(files));
    check(back == files, "Roland: las asignaciones se guardan y se leen igual");
    check(RolandSound::parseAssignments("zz = 3\n12 = 300\n13 = bateria 99\n").empty(),
          "Roland: las líneas sin sentido se saltan");
}

void testVgm() {
    // Record the test ROM's tune, then play the file: the same sound comes out.
    MegaDrive md;
    md.loadRomData(testrom::build());
    md.setSampleRate(44100);
    VgmWriter writer;
    for (int i = 0; i < 10; ++i) md.runFrame();  // the tune has started: its note is held
    md.audio().clearSamples();
    md.setVgmWriter(&writer);
    std::vector<int16_t> live;
    for (int i = 0; i < 120; ++i) {
        md.runFrame();
        live.insert(live.end(), md.audio().samples().begin(), md.audio().samples().end());
        md.audio().clearSamples();
    }
    const std::vector<uint8_t> bytes = writer.finish(md.masterClocks(), "Prueba");
    md.setVgmWriter(nullptr);
    check(bytes.size() > 18 && bytes[0] == 0x1F && bytes[1] == 0x8B, "VGM: la grabación se guarda comprimida (.vgz)");
    VgmFile file;
    std::string error;
    check(file.parse(bytes, &error), "VGM: la grabación se lee");
    check(file.game == "Prueba" && std::fabs(file.seconds() - 2.0) < 0.05 && file.ymClock > 7600000,
          "VGM: dura lo grabado y dice de qué juego es");
    VgmPlayer player;
    RolandSound roland;
    player.setRoland(&roland);
    player.setRolandOutput(false);
    player.load(file);
    player.analyse();
    check(roland.seen().size() == 1, "VGM: el análisis encuentra el instrumento de la nota que ya sonaba");
    std::vector<int16_t> played;
    while (!player.finished()) player.play(1.0 / 60, 44100, played);
    // How loud, leaving out the constant offset the YM2612's DAC ladder adds even in silence.
    auto rms = [](const std::vector<int16_t>& s) {
        if (s.empty()) return 0.0;
        double mean = 0, sum = 0;
        for (int16_t v : s) mean += v;
        mean /= static_cast<double>(s.size());
        for (int16_t v : s) sum += (v - mean) * (v - mean);
        return std::sqrt(sum / static_cast<double>(s.size()));
    };
    check(rms(live) > 500 && std::fabs(rms(played) - rms(live)) < rms(live) * 0.05,
          "VGM: al tocarla suena como el juego");
    player.restart();
    player.setListening(1, {});  // FM 1 muted
    std::vector<int16_t> muted;
    for (int i = 0; i < 60; ++i) player.play(1.0 / 60, 44100, muted);
    check(rms(muted) < 50, "VGM: silenciar el canal que suena deja la pista callada");
    check(!file.parse({1, 2, 3}, &error) && !error.empty(), "VGM: un archivo que no lo es se rechaza");
}

void testSongExtract(testrom::Asking asking) {
    // The music test ROM asks for its songs like Sonic does (or like Streets of Rage 2,
    // like EA's games or like the GEMS games): the extractor finds how, and takes out its four songs, each
    // once round with its loop.
    const std::string path = (std::filesystem::temp_directory_path() / "plumsg-music-test.md").string();
    {
        const std::vector<uint8_t> rom = testrom::buildMusic(asking);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }
    const std::string how = asking == testrom::Asking::byCall    ? " (llamando a una rutina)"
                            : asking == testrom::Asking::byStack ? " (con el número en la pila)"
                            : asking == testrom::Asking::bySlot  ? " (con el número en su sitio de la pila)"
                            : asking == testrom::Asking::byRing  ? " (con mensajes en un anillo)"
                                                                 : "";
    const std::string said = asking == testrom::Asking::byCall    ? "Pido las canciones llamando a"
                             : asking == testrom::Asking::byStack ? "Pido las canciones llamando a"
                             // (The call only on the second boot: the byte the routine writes first.)
                             : asking == testrom::Asking::bySlot  ? "Pido las canciones en"
                             : asking == testrom::Asking::byRing  ? "Pido las canciones con el mensaje FF 10 n"
                                                                  : "Pido las canciones en";
    SongExtractor extractor;
    std::vector<std::string> log;
    extractor.setLog([&](const std::string& s) { log.push_back(s); });
    std::vector<SongExtractor::Song> songs;
    std::string error;
    const bool ok = extractor.run(path, "Prueba", nullptr, [&](const SongExtractor::Song& s) { songs.push_back(s); }, &error);
    for (const std::string& s : log) std::printf("     %s\n", s.c_str());
    bool asked = false;
    for (const std::string& s : log) asked = asked || s.rfind(said, 0) == 0;
    check(ok && asked, ("Canciones: la extracción encuentra cómo pide la música el juego" + how).c_str());
    check(songs.size() == 4 && songs[0].number == 0x81 && songs[3].number == 0x84,
          ("Canciones: salen las cuatro canciones y ningún efecto" + how).c_str());
    bool loops = !songs.empty();
    for (const auto& s : songs) loops = loops && std::fabs(s.loopSeconds - 8.0) < 0.1 && s.seconds < 9.0;
    check(loops, ("Canciones: cada una dura una vuelta, con su punto de vuelta" + how).c_str());
    // The sound effects play an instrument no song plays: that one is an effect.
    const std::set<uint32_t> effects = extractor.effectInstruments();
    check(effects.size() == 1, ("Canciones: el instrumento de los efectos se reconoce como efecto" + how).c_str());
    std::filesystem::remove(path);
    if (asking != testrom::Asking::byQueue) return;
    VgmFile file;
    check(!songs.empty() && file.parse(songs[1].vgz, &error) && file.title == "Canción 2" && file.loop != 0,
          "Canciones: el archivo dice su título y dónde vuelve");
    // And it keeps its chip sound when effects are to sound original.
    RolandSound roland;
    roland.setSoundEffects(effects, true);
    check(!effects.empty() && roland.assignmentFor(*effects.begin()).original,
          "Canciones: con efectos originales, el efecto suena como en la consola");
    roland.setSoundEffects(effects, false);
    check(!effects.empty() && !roland.assignmentFor(*effects.begin()).original,
          "Canciones: con efectos Roland, el efecto pasa por el SoundFont");
}

// The cartridge with four more bytes at its end that give the whole of it this CRC32:
// the CRC is affine in those bits, so they are solved for over GF(2).
std::vector<uint8_t> withCrc32(std::vector<uint8_t> rom, uint32_t target) {
    rom.resize(rom.size() + 4, 0);
    const auto crc = [&] { return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, rom.data(), rom.size())); };
    const size_t at = rom.size() - 4;
    const uint32_t base = crc();
    uint32_t cols[32];
    for (int b = 0; b < 32; ++b) {
        rom[at + b / 8] = static_cast<uint8_t>(1 << (b % 8));
        cols[b] = crc() ^ base;
        rom[at + b / 8] = 0;
    }
    // Rows of [cols | want]: row i is bit i of every column, and of the CRC wanted.
    uint64_t rows[32];
    for (int i = 0; i < 32; ++i) {
        rows[i] = ((target ^ base) >> i & 1ull) << 32;
        for (int b = 0; b < 32; ++b) rows[i] |= static_cast<uint64_t>(cols[b] >> i & 1) << b;
    }
    for (int b = 0, r = 0; b < 32; ++b) {
        int p = r;
        while (p < 32 && !(rows[p] >> b & 1)) ++p;
        if (p == 32) continue;
        std::swap(rows[p], rows[r]);
        for (int i = 0; i < 32; ++i)
            if (i != r && (rows[i] >> b & 1)) rows[i] ^= rows[r];
        ++r;
    }
    for (int i = 0; i < 32; ++i)
        for (int b = 0; b < 32; ++b)
            if (rows[i] & (1ull << b)) {
                if (rows[i] >> 32 & 1) rom[at + b / 8] |= static_cast<uint8_t>(1 << (b % 8));
                break;
            }
    return rom;
}

void testMasterSystem() {
    namespace st = smstestrom;
    {
        // Back to the Future Part III only runs on a 50 Hz console, though its header
        // says "export" as the American games do (issue #89).
        const std::vector<uint8_t> bttf = withCrc32(st::build(), 0x2D48C1D3);
        MegaDrive md;
        md.loadRomData(bttf);
        check(mz_crc32(MZ_CRC32_INIT, bttf.data(), bttf.size()) == 0x2D48C1D3 && smsRegion(bttf) == Region::EuropePal &&
                  md.isMasterSystem() && md.pal() && md.masterSystem()->vdp().pal() && !md.masterSystem()->fmPresent(),
              "Master System: a game that needs 50 Hz runs on a European console");
    }
    const std::vector<uint8_t> rom = st::build();
    check(detectSystemFromData(rom) == ConsoleSystem::MasterSystem && smsRegion(rom) == Region::UsaNtsc,
          "Master System: detected from its header");
    MegaDrive md;
    md.loadRomData(rom);
    md.setSampleRate(44100);
    check(md.isMasterSystem() && !md.is32x() && !md.isMegaCd(), "Master System: runs in its mode");
    std::vector<int16_t> sound;
    for (int i = 0; i < 60; ++i) {
        md.runFrame();
        sound.insert(sound.end(), md.audio().samples().begin(), md.audio().samples().end());
        md.audio().clearSamples();
    }
    const VideoFrameMd& f = md.frame();
    check(f.width == 256 && f.height == 192 && f.wideLeft == 0 && f.wideRight == 0, "Master System: 256x192");
    const int frames = md.z80Ram()[0];
    check(frames >= 55 && frames <= 60, "Master System: one frame interrupt a frame");
    // The scroll was set by the interrupt during the last frame, so the picture shows the
    // value before it.
    const int h = f.lines[0].hscrollB;
    check(h == ((frames - 1) & 0xFF), "Master System: the interrupt scrolls the background");
    bool halves = true;
    for (int x = 8; x < 256; ++x) halves &= f.pixel(x, 20) == (((x - h) & 7) < 4 ? st::kRed : st::kBlue);
    check(halves, "Master System: background tiles, palette and scroll");
    check(f.pixel(0, 20) == st::kYellow && f.pixel(7, 100) == st::kYellow &&
              f.info[20 * VideoFrameMd::kMaxWidth + 3].layer == MdLayer::Backdrop,
          "Master System: left column masked with the backdrop");
    const int sx = st::kSpriteLeft + 2;
    check(f.pixel(sx, st::kSpriteTop + 7) == st::kMagenta &&
              f.info[(st::kSpriteTop + 7) * VideoFrameMd::kMaxWidth + sx].layer == MdLayer::Sprite,
          "Master System: a sprite");
    const int under = (st::kSpriteTop + 2) * VideoFrameMd::kMaxWidth + sx;
    check(f.pixel(sx, st::kSpriteTop + 2) == st::kGreen && f.info[under].layer == MdLayer::PlaneA &&
              (f.sprites[under] >> 24) != 0,
          "Master System: a tile with priority in front of the sprite, the sprite still in its layer");
    check(f.info[(st::kSpriteTop - 2) * VideoFrameMd::kMaxWidth + sx].layer != MdLayer::Sprite &&
              f.info[(st::kSpriteTop + 8) * VideoFrameMd::kMaxWidth + sx].layer != MdLayer::Sprite,
          "Master System: the sprite is only on its lines");
    double sumSq = 0;
    for (int16_t s : sound) sumSq += static_cast<double>(s) * s;
    check(sound.size() / 2 > 43000 && sound.size() / 2 < 45000 && std::sqrt(sumSq / static_cast<double>(sound.size())) > 300,
          "Master System: the PSG plays its tone");

    // Pads (active low on port 0xDC) and the pause button.
    PadState p;
    p.up = p.b = true;
    md.setPads(p, PadState{});
    md.runFrame();
    check(md.z80Ram()[2] == static_cast<uint8_t>(~0x11), "Master System: up and button 1 on port 0xDC");
    p = PadState{};
    p.c = p.right = true;
    md.setPads(p, PadState{});
    md.runFrame();
    check(md.z80Ram()[2] == static_cast<uint8_t>(~0x28), "Master System: right and button 2 on port 0xDC");
    p = PadState{};
    p.start = true;
    md.setPads(p, PadState{});
    md.runFrame();
    md.runFrame();  // held: still one press
    md.setPads(PadState{}, PadState{});
    md.runFrame();
    check(md.z80Ram()[1] == 1, "Master System: Start is the pause button (one NMI a press)");

    // Savestates.
    std::vector<uint8_t> state;
    check(md.saveState(state), "Master System: state saved");
    auto run = [&](std::vector<uint32_t>& hashes) {
        for (int i = 0; i < 30; ++i) {
            md.runFrame();
            hashes.push_back(frameHash(md.frame()));
            md.audio().clearSamples();
        }
    };
    std::vector<uint32_t> h1, h2;
    run(h1);
    std::string error;
    check(md.loadState(state, &error), "Master System: state loaded");
    run(h2);
    check(h1 == h2, "Master System: the same frames after loading a state");
    std::vector<uint32_t> pixels;
    int tw = 0, th = 0;
    check(MegaDrive::stateThumbnail(state, pixels, &tw, &th) && tw == 128 && th == 96, "Master System: thumbnail");

    // A Mega Drive game afterwards leaves the mode.
    md.loadRomData(testrom::build());
    for (int i = 0; i < 3; ++i) md.runFrame();
    check(!md.isMasterSystem() && md.frame().width == 320, "Master System: a Mega Drive game leaves the mode");
}

void testSmsMapper() {
    // 128 KB: bank n has n in every byte, after its first KB.
    std::vector<uint8_t> rom(0x20000);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<uint8_t>(i / 0x4000);
    std::copy_n("TMR SEGA", 8, rom.begin() + 0x7FF0);
    Cartridge cart;
    cart.loadData(rom);
    std::array<uint8_t, 0x2000> ram{};
    auto frame = std::make_unique<VideoFrameMd>();
    MasterSystem sms(cart, ram, *frame);
    sms.powerOn();
    check(sms.read(0x0000) == 0 && sms.read(0x4000) == 1 && sms.read(0x8000) == 2, "SMS mapper: banks 0, 1, 2 at power on");
    sms.write(0xFFFF, 7);
    sms.write(0xFFFE, 5);
    sms.write(0xFFFD, 3);
    check(sms.read(0x8000) == 7 && sms.read(0x4000) == 5 && sms.read(0x0400) == 3 && sms.read(0x0000) == 0,
          "SMS mapper: banks switched, the first KB stays");
    sms.write(0xFFFF, 9);
    check(sms.read(0x8000) == 1, "SMS mapper: bank numbers wrap around the ROM");
    check(ram[0x1FFF] == 9 && sms.read(0xDFFF) == 9 && sms.read(0xFFFF) == 9, "SMS mapper: registers are in RAM too");
    sms.write(0xFFFC, 0x08);
    sms.write(0x8000, 0x5A);
    sms.write(0xFFFC, 0x0C);
    sms.write(0x8000, 0xA5);
    check(sms.read(0x8000) == 0xA5 && cart.saveRamByte(0x4000) == 0xA5 && cart.saveRamByte(0) == 0x5A &&
              cart.hasSaveData(),
          "SMS mapper: two pages of battery RAM");
    sms.write(0xFFFC, 0);
    check(sms.read(0x8000) == 1, "SMS mapper: back to ROM");
    // The VDP: a register write, an address, VRAM through the data port and the read buffer.
    sms.out(0xBF, 0x12);
    sms.out(0xBF, 0x87);
    check(sms.vdp().reg(7) == 0x12, "SMS VDP: register write");
    sms.out(0xBF, 0x00);
    sms.out(0xBF, 0x41);
    sms.out(0xBE, 0xAB);
    sms.out(0xBE, 0xCD);
    sms.out(0xBF, 0x00);
    sms.out(0xBF, 0x01);
    check(sms.in(0xBE, 0) == 0xAB && sms.in(0xBE, 0) == 0xCD, "SMS VDP: VRAM written and read back");
    // The H counter port keeps what TH going high latched: 0xF4 where the line starts,
    // 0x93 and then 0xE9 across the jump.
    sms.out(0x3F, 0x55, 0);
    sms.out(0x3F, 0xFF, 0);
    check(sms.in(0x7F, 0) == 0xF4 && sms.in(0x7F, 100) == 0xF4, "SMS VDP: H counter latched by TH");
    sms.out(0x3F, 0x55, 196 - 212 + 228);
    sms.out(0x3F, 0xFF, 196 - 212 + 228);
    const uint8_t before = sms.in(0x7F, 0);
    sms.out(0x3F, 0x55, 197 - 212 + 228);
    sms.out(0x3F, 0xFF, 197 - 212 + 228);
    check(before == 0x93 && sms.in(0x7F, 0) == 0xE9, "SMS VDP: H counter jumps from 0x93 to 0xE9");
}

void testSmsPhase2() {
    // The FM unit: a note on the YM2413 sounds.
    Ym2413 opll;
    opll.setMasterClock(MegaDrive::kMasterNtsc);
    auto reg = [&](int r, int v) {
        opll.write(0, static_cast<uint8_t>(r));
        opll.write(1, static_cast<uint8_t>(v));
    };
    reg(0x30, 0x10);  // channel 0: instrument 1 (violin), loudest
    reg(0x10, 0xAC);
    reg(0x20, 0x18);  // key on, block 4
    opll.runUntil(15 * 72 * 4000);
    double sumSq = 0;
    for (int16_t v : opll.samples()) sumSq += static_cast<double>(v) * v;
    check(opll.samples().size() == 8000 && std::sqrt(sumSq / 8000) > 200 && opll.reg(0x30) == 0x10,
          "YM2413: a note on the FM unit");

    // It is there on Japanese consoles (or always, if asked), and answers on port 0xF2.
    MegaDrive md;
    md.loadRomData(smstestrom::build());
    check(md.isMasterSystem() && !md.masterSystem()->fmPresent() && md.ym2413(), "Master System FM: not on export consoles");
    md.setRegion(Region::JapanNtsc);
    md.powerCycle();
    check(md.masterSystem()->fmPresent(), "Master System FM: on Japanese consoles");
    md.setRegion(Region::UsaNtsc);
    md.setMasterSystemFm(true);
    check(md.masterSystem()->fmPresent(), "Master System FM: on any console if asked");
    md.setPads(PadState{}, PadState{});
    md.runFrame();
    std::vector<uint8_t> state;
    std::string error;
    check(md.saveState(state) && md.loadState(state, &error), "Master System FM: in the savestates");

    // Codemasters mapper: slots at 0x0000, 0x4000, 0x8000, the first one too.
    std::vector<uint8_t> rom(0x40000);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<uint8_t>(i / 0x4000);
    rom[0x7FE6] = 0x34;
    rom[0x7FE7] = 0x12;
    rom[0x7FE8] = 0xCC;
    rom[0x7FE9] = 0xED;  // 0x1234 + 0xEDCC = 0x10000
    Cartridge cart;
    cart.loadData(rom);
    std::array<uint8_t, 0x2000> ram{};
    auto frame = std::make_unique<VideoFrameMd>();
    MasterSystem sms(cart, ram, *frame);
    sms.powerOn();
    check(sms.codemasters() && sms.read(0x0000) == 0 && sms.read(0x4000) == 1 && sms.read(0x8000) == 0,
          "Codemasters mapper: detected, banks 0, 1, 0");
    sms.write(0x0000, 5);
    sms.write(0x4000, 0x86);
    sms.write(0x8000, 15);
    sms.write(0xA000, 0x77);
    check(sms.read(0x0000) == 5 && sms.read(0x4000) == 6 && sms.read(0x8000) == 15 && sms.read(0xA000) == 0x77 &&
              cart.saveRamByte(0) == 0x77,
          "Codemasters mapper: three slots and the RAM of Ernie Els Golf");
    // Codemasters made their Master System games for Europe only (issue #74).
    check(smsRegion(rom) == Region::EuropePal && smsRegion(rom, true) == Region::UsaNtsc,
          "Codemasters: a European console for the Master System, not for the Game Gear");

    // The TMS9918's Graphics I mode with a sprite.
    MasterSystem tms(cart, ram, *frame);
    tms.powerOn();
    auto vdpReg = [&](int r, int v) {
        tms.out(0xBF, static_cast<uint8_t>(v));
        tms.out(0xBF, static_cast<uint8_t>(0x80 | r));
    };
    auto vram = [&](int addr, std::initializer_list<int> bytes) {
        tms.out(0xBF, static_cast<uint8_t>(addr));
        tms.out(0xBF, static_cast<uint8_t>(0x40 | addr >> 8));
        for (int b : bytes) tms.out(0xBE, static_cast<uint8_t>(b));
    };
    vdpReg(0, 0x00);
    vdpReg(1, 0x40);  // display on, Graphics I
    vdpReg(2, 0x06);  // names 0x1800
    vdpReg(3, 0x80);  // colours 0x2000
    vdpReg(4, 0x00);  // patterns 0x0000
    vdpReg(5, 0x76);  // sprites 0x3B00
    vdpReg(6, 0x00);
    vdpReg(7, 0x04);  // backdrop dark blue
    vram(0x0008, {0xF0});        // pattern 1, row 0: left half set
    vram(0x0010, {0xFF, 0xFF});  // pattern 2: the sprite, rows 0-1
    vram(0x1800, {0x01});
    vram(0x2000, {0x2F});        // patterns 0-7: medium green on white
    vram(0x3B00, {0x00, 16, 2, 0x09, 0xD0});
    for (int line = 0; line < 3; ++line) tms.vdp().startLine(line, true);
    const uint32_t green = 0xFF00AA00, white = 0xFFFFFFFF, lightRed = 0xFFFF0000;
    check(frame->pixel(0, 0) == green && frame->pixel(4, 0) == white && frame->pixel(8, 1) == white &&
              frame->pixel(16, 1) == lightRed && frame->info[VideoFrameMd::kMaxWidth + 16].layer == MdLayer::Sprite &&
              frame->pixel(16, 0) != lightRed,
          "SMS VDP: TMS9918 Graphics I with a sprite");
}

void testGameGear() {
    namespace st = smstestrom;
    const std::vector<uint8_t> rom = st::build(true);
    check(detectSystemFromData(rom) == ConsoleSystem::GameGear, "Game Gear: detected from its header");
    MegaDrive md;
    md.loadRomData(rom);
    md.setSampleRate(44100);
    check(md.isGameGear() && md.isMasterSystem(), "Game Gear: runs in its mode");
    std::vector<int16_t> sound;
    for (int i = 0; i < 60; ++i) {
        md.runFrame();
        sound.insert(sound.end(), md.audio().samples().begin(), md.audio().samples().end());
        md.audio().clearSamples();
    }
    const VideoFrameMd& f = md.frame();
    check(f.width == 160 && f.height == 144 && f.aspectLines == 144, "Game Gear: a 160x144 screen");
    const int h = f.lines[0].hscrollB;
    bool halves = true;
    for (int x = 0; x < 160; ++x) halves &= f.pixel(x, 10) == (((x + st::kGgLeft - h) & 7) < 4 ? st::kRed : st::kBlue);
    check(halves, "Game Gear: the middle of the picture, in its 12-bit colours");
    const int sx = st::kSpriteLeft + 2 - st::kGgLeft, sy = st::kSpriteTop + 7 - st::kGgTop;
    check(f.pixel(sx, sy) == st::kMagenta && f.pixel(sx, sy - 5) == st::kGreen &&
              f.info[sy * VideoFrameMd::kMaxWidth + sx].layer == MdLayer::Sprite,
          "Game Gear: the sprite and the row in front of it");
    double left = 0, right = 0;
    for (size_t i = 0; i + 1 < sound.size(); i += 2) {
        left += std::abs(sound[i]);
        right += std::abs(sound[i + 1]);
    }
    check(left > 100 * right + 1, "Game Gear: the PSG's stereo (left only)");

    PadState p;
    p.start = true;
    p.b = true;
    md.setPads(p, p);
    md.runFrame();
    check(md.z80Ram()[3] == 0x40 && md.z80Ram()[2] == static_cast<uint8_t>(~0x10) && md.z80Ram()[1] == 0,
          "Game Gear: START on port 0x00, one pad, no pause button");
    std::vector<uint8_t> state;
    std::string error;
    md.saveState(state);
    std::vector<uint32_t> h1, h2;
    for (int i = 0; i < 20; ++i) {
        md.runFrame();
        h1.push_back(frameHash(md.frame()));
    }
    check(md.loadState(state, &error), "Game Gear: state loaded");
    for (int i = 0; i < 20; ++i) {
        md.runFrame();
        h2.push_back(frameHash(md.frame()));
    }
    check(h1 == h2, "Game Gear: the same frames after loading a state");
    md.loadRomData(st::build());
    md.runFrame();
    check(!md.isGameGear() && md.frame().width == 256 && md.frame().aspectLines == 224, "Game Gear: back to a Master System");
}

void testSmsCheats() {
    CheatPatch p;
    std::string error;
    check(decodeCheat("00C0-1255", &p, &error, true) && p.address == 0xC012 && p.value == 0x55 && p.byte &&
              p.compare < 0,
          "SMS cheats: Action Replay");
    check(!decodeCheat("0012-3455", &p, &error, true), "SMS cheats: Action Replay outside RAM refused");
    check(decodeCheat("3A8-6CF", &p, &error, true) && p.address == 0x086C && p.value == 0x3A && p.compare < 0,
          "SMS cheats: Game Genie without a compare byte");
    check(decodeCheat("3a8-6cf-e66", &p, &error, true) && p.address == 0x086C && p.value == 0x3A && p.compare == 0x03,
          "SMS cheats: Game Genie with a compare byte");
    check(!decodeCheat("ABCD-EFGH", &p, &error, true), "SMS cheats: Mega Drive codes refused");

    namespace st = smstestrom;
    MegaDrive md;
    md.loadRomData(st::build());
    std::vector<CheatPatch> cheats;
    check(decodeCheatCodes("00C0-1042", &cheats, &error, true), "SMS cheats: list decoded");
    const uint8_t original = md.masterSystem()->read(0x0001);
    CheatPatch rom;
    rom.address = 0x0001;
    rom.value = static_cast<uint8_t>(original ^ 0xFF);
    rom.byte = true;
    rom.compare = original;
    cheats.push_back(rom);
    md.setCheats(cheats);
    md.runFrame();
    check(md.z80Ram()[0x10] == 0x42, "SMS cheats: the RAM byte held");
    check(md.masterSystem()->read(0x0001) == static_cast<uint8_t>(original ^ 0xFF), "SMS cheats: the ROM byte patched");
    md.setCheats({});
    check(md.masterSystem()->read(0x0001) == original, "SMS cheats: the ROM byte back");
}

void testSmsVgm() {
    // A Master System recording: the YM2413's writes and the PSG, no YM2612.
    Psg psg;
    Ym2413 opll;
    opll.setMasterClock(MegaDrive::kMasterNtsc);
    VgmWriter writer;
    writer.beginSms(psg, &opll, false, 0, MegaDrive::kMasterNtsc);
    const uint64_t second = static_cast<uint64_t>(MegaDrive::kMasterNtsc);
    for (auto [r, v] : std::initializer_list<std::pair<int, int>>{{0x30, 0x10}, {0x10, 0x80}, {0x20, 0x1C}}) {
        writer.opll(0, static_cast<uint8_t>(r), second / 10);
        writer.opll(1, static_cast<uint8_t>(v), second / 10);
    }
    VgmFile file;
    std::string error;
    check(file.parse(writer.finish(second, "Prueba SMS"), &error) && file.opllClock > 3500000 && file.ymClock == 0 &&
              file.psgClock > 0 && std::fabs(file.seconds() - 1.0) < 0.01,
          "SMS VGM: a recording with the YM2413 and the PSG, no YM2612");
    VgmPlayer player;
    player.load(file);
    std::vector<int16_t> played;
    while (!player.finished()) player.play(1.0 / 60, 44100, played);
    double sumSq = 0;
    for (size_t i = played.size() / 2; i < played.size(); ++i) sumSq += static_cast<double>(played[i]) * played[i];
    check(std::sqrt(sumSq / static_cast<double>(played.size() / 2)) > 200, "SMS VGM: its FM note plays back");

    // A Game Gear one keeps its stereo.
    writer.beginSms(psg, nullptr, true, 0, MegaDrive::kMasterNtsc);
    writer.ggStereo(0xF0, second / 2);
    std::vector<uint8_t> bytes = writer.finish(second, "Prueba GG", false);
    bool stereo = false;
    for (size_t i = 0x40; i + 1 < bytes.size(); ++i) stereo |= bytes[i] == 0x4F && bytes[i + 1] == 0xF0;
    check(file.parse(bytes, &error) && file.opllClock == 0 && file.ymClock == 0 && stereo,
          "SMS VGM: a Game Gear recording with its stereo");
}

void testSmsRoland() {
    // The YM2413's notes as General MIDI: its built-in instruments, its game's own and its drums.
    Ym2413 opll;
    opll.setMasterClock(MegaDrive::kMasterNtsc);
    RolandSound roland;
    std::vector<std::string> log;
    roland.setLog(&log);
    auto fm = [&](int reg, int value) {
        opll.write(0, static_cast<uint8_t>(reg));
        roland.opllWrite(0, static_cast<uint8_t>(reg), opll, 0);
        opll.write(1, static_cast<uint8_t>(value));
        roland.opllWrite(1, static_cast<uint8_t>(value), opll, 0);
    };
    auto has = [&](const std::string& line) { return std::find(log.begin(), log.end(), line) != log.end(); };
    check(std::fabs(RolandSound::opllNote(289, 4, MegaDrive::kMasterNtsc / 15) - 69.0) < 0.1,
          "SMS Roland: F-number 289 block 4 is the A 440");
    fm(0x30, 0x10);  // channel 1: violin, full volume
    fm(0x10, 289 & 0xFF);
    fm(0x20, 0x18 | (289 >> 8));  // key on, block 4
    check(has("0 program 40") && has("0 on 69"), "SMS Roland: the YM2413's violin plays a GM violin");
    check(RolandSound::kindOf(RolandSound::opllFingerprint(1)) == RolandSound::Kind::Fm &&
              roland.seen().count(RolandSound::opllFingerprint(1)),
          "SMS Roland: the built-in instrument is seen");
    fm(0x20, 0x08 | (289 >> 8));  // key off
    check(has("0 off 69"), "SMS Roland: key off ends the note");
    log.clear();
    fm(0x38, 0x10);  // channel 9: violin too
    fm(0x18, 289 & 0xFF);
    fm(0x28, 0x18 | (289 >> 8));
    check(has("13 on 69"), "SMS Roland: channel 9 plays on its own MIDI channel");
    log.clear();
    fm(0x0E, 0x30);  // rhythm mode: bass drum
    check(has("13 off 69") && has("9 on 36"), "SMS Roland: rhythm mode stops channel 9 and hits the bass drum");
    log.clear();
    fm(0x0E, 0x30);
    check(log.empty(), "SMS Roland: a drum held is one hit");
    fm(0x0E, 0x21);  // hi-hat
    check(has("9 on 42"), "SMS Roland: the hi-hat");
    for (int r = 0; r < 8; ++r) fm(r, 0x21 + r);  // the game's own instrument
    fm(0x31, 0x00);
    fm(0x11, 0x80);
    fm(0x21, 0x14);  // block 2: low
    bool custom = false;
    for (const auto& [fp, seen] : roland.seen())
        custom |= seen.kind == RolandSound::Kind::Fm && !RolandSound::opllInstrument(fp) && seen.channels == 2;
    check(custom, "SMS Roland: the game's own instrument is an FM instrument of its own");

    RolandSound::Assignments original;
    original[RolandSound::rhythmFingerprint(0)].original = true;
    roland.setAssignments(original);
    fm(0x0E, 0x20);
    fm(0x0E, 0x30);
    check(RolandSound::opllChannels(RolandSound::kAllVoices & ~roland.chipChannels()) == (0x1FF & ~0x40),
          "SMS Roland: a drum left original keeps its channel on the chip");

    // The Game Gear's stereo pans the PSG.
    Psg psg;
    psg.setStereo(0xE1);  // channel 1 right only
    log.clear();
    psg.write(0x8E);
    psg.write(0x0F);
    psg.write(0x90);
    roland.psgWrite(psg, 0);
    check(has("6 pan 127") && has("6 on 69"), "SMS Roland: a Game Gear channel on the right is panned right");
}

void testSmsSongExtract() {
    // The Master System music test ROM: its driver takes requests from a byte of RAM.
    const std::string path = (std::filesystem::temp_directory_path() / "plumsg-music-test.sms").string();
    {
        const std::vector<uint8_t> rom = smstestrom::buildMusic();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }
    SongExtractor extractor;
    std::vector<std::string> log;
    extractor.setLog([&](const std::string& s) { log.push_back(s); });
    std::vector<SongExtractor::Song> songs;
    std::string error;
    const bool ok = extractor.run(path, "Prueba SMS", nullptr, [&](const SongExtractor::Song& s) { songs.push_back(s); }, &error);
    for (const std::string& s : log) std::printf("     %s\n", s.c_str());
    if (!ok) std::printf("     %s\n", error.c_str());
    bool asked = false;
    for (const std::string& s : log) asked = asked || s.rfind("Pido las canciones en 00C010", 0) == 0;
    check(ok && asked, "SMS songs: the extractor finds the request byte");
    check(songs.size() == 4 && songs[0].number == 0x81 && songs[3].number == 0x84, "SMS songs: the four songs come out");
    bool loops = !songs.empty();
    for (const auto& s : songs) loops = loops && std::fabs(s.loopSeconds - 8.0) < 0.1 && s.seconds < 9.0;
    check(loops, "SMS songs: each one round, with its loop");
    VgmFile file;
    check(!songs.empty() && file.parse(songs[0].vgz, &error) && file.ymClock == 0 && file.psgClock > 0,
          "SMS songs: a Master System VGM");
    std::filesystem::remove(path);
}

int main() {
    testHeader();
    testSmd();
    testZipRom();
    testFolderAsRom();
    testSram();
    testEeprom();
    testPads();
    testPsg();
    testVdpPorts();
    testVdpHblankFlag();
    testVdpSpriteMaskAfterDotOverflow();
    testVdpDirectColor();
    testVdpFillScrollTable();
    testVdpSpriteLimit();
    testVdpWidescreen();
    testOverclock();
    testBusAckOpenBus();
    testPrefetch();
    testZ80ResetKeepsRegisters();
    testTraceSkipsIllegal();
    testVintFlagWait();
    testSplitRoms();
    testTestRom();
    testStates();
    testRewindBuffer();
    testFramePacer();
    testRewindPlayback();
    testRunAhead();
    testPlanes();
    testProfiles();
    testLiveCamera();
    testEdgeDistance();
    testPal();
    testCheats();
    testLibraryNames();
    testGameInfo();
    testSystems();
    testMasterSystem();
    testSmsMapper();
    testSmsPhase2();
    testGameGear();
    testSmsCheats();
    testSmsVgm();
    testSmsRoland();
    testSmsSongExtract();
    testSh2();
    testMars();
    testMarsResetButton();
    testMarsProgram();
    testCdImage();
    testMegaCd();
    testMegaCdBootRom();
    testMsuMd();
    testPaprium();
    testSvp();
    testVdpInstantDma();
    testNetplay();
    testNotation();
    testRoland();
    testAchievements();
    testVgm();
    testSongExtract(testrom::Asking::byQueue);
    testSongExtract(testrom::Asking::byCall);
    testSongExtract(testrom::Asking::byStack);
    testSongExtract(testrom::Asking::bySlot);
    testSongExtract(testrom::Asking::byRing);
    {
        // The Z80's reset also resets the YM2612; what it already played must stay.
        Ym2612 ym;
        ym.setMasterClock(MegaDrive::kMasterNtsc);
        ym.runUntil(7 * 144 * 100);
        ym.reset();
        check(ym.samples().size() == 200, "YM2612: un reset no tira el sonido ya hecho");
    }
    std::printf(failures ? "%d fallos\n" : "todo bien\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
