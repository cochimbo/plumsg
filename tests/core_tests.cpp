// Core tests that need no external files: the cartridge (header, SMD, zip, SRAM), the
// pads, the PSG, the VDP ports, the built-in test ROM drawn and heard, savestates
// (determinism, round trip, damaged files), rewinding and the speed controls, the
// pseudo-3D planes, the games' 3D profiles, the cheat codes and the library's names.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "megadrive.h"
#include "miniz.h"
#include "render/planes.h"
#include "render/profile3d.h"
#include "rewind.h"
#include "test_rom.h"
#include "frontend/library_names.h"

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
        mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
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

void testSram() {
    // A 1 MB ROM with no SRAM in its header still gets the usual 64 KB at 0x200000.
    std::vector<uint8_t> rom = testrom::build();
    rom.resize(0x100000, 0xFF);
    Cartridge cart;
    cart.loadData(rom);
    cart.write8(0x200001, 0x5A);
    check(cart.read8(0x200001) == 0x5A && cart.hasSaveData(), "SRAM at 0x200000 for small ROMs");
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
    check(!decodeCheat("FF0A13:0009", &p, &error), "cheats: word write to an odd address refused");
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
}

int main() {
    testHeader();
    testSmd();
    testZipRom();
    testSram();
    testPads();
    testPsg();
    testVdpPorts();
    testVdpHblankFlag();
    testVdpFillScrollTable();
    testTestRom();
    testStates();
    testRewindBuffer();
    testFramePacer();
    testRewindPlayback();
    testPlanes();
    testProfiles();
    testLiveCamera();
    testEdgeDistance();
    testPal();
    testCheats();
    testLibraryNames();
    std::printf(failures ? "%d fallos\n" : "todo bien\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
