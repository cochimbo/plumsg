#pragma once

// Builds a small Mega Drive program used by the tests and tools in place of a
// commercial ROM (none can be committed). It sets up the VDP for H40, loads tiles,
// palettes, both planes and a sprite table, starts a 440 Hz tone on the YM2612 and
// scrolls plane B one pixel per frame from its VBlank interrupt. Every layer the
// pseudo-3D renderer separates (backdrop, plane B, plane A, sprites, high priority)
// shows something.

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace plumsg::testrom {

class Emitter {
public:
    std::vector<uint8_t> rom = std::vector<uint8_t>(0x200, 0);

    uint32_t here() const { return static_cast<uint32_t>(rom.size()); }
    void w16(uint32_t v) {
        rom.push_back(static_cast<uint8_t>(v >> 8));
        rom.push_back(static_cast<uint8_t>(v));
    }
    void w32(uint32_t v) {
        w16(v >> 16);
        w16(v & 0xFFFF);
    }
    void label(const std::string& name) { labels_[name] = here(); }
    // A 16-bit PC-relative displacement to a label, resolved by link().
    void rel(const std::string& name) {
        fixups_.push_back({here(), name});
        w16(0);
    }
    void put16(uint32_t at, uint32_t v) {
        rom[at] = static_cast<uint8_t>(v >> 8);
        rom[at + 1] = static_cast<uint8_t>(v);
    }
    void put32(uint32_t at, uint32_t v) {
        put16(at, v >> 16);
        put16(at + 2, v & 0xFFFF);
    }
    uint32_t at(const std::string& name) const { return labels_.at(name); }
    void link() {
        for (const auto& f : fixups_) put16(f.first, (labels_.at(f.second) - f.first) & 0xFFFF);
    }

private:
    std::map<std::string, uint32_t> labels_;
    std::vector<std::pair<uint32_t, std::string>> fixups_;
};

inline uint16_t rgb(int r, int g, int b) { return static_cast<uint16_t>(b << 9 | g << 5 | r << 1); }

// 8x8 tile, 4 bits per pixel, from a function of (x, y).
template <class F>
std::vector<uint16_t> tile(F pixel) {
    std::vector<uint16_t> words;
    for (int y = 0; y < 8; ++y)
        for (int half = 0; half < 2; ++half) {
            uint16_t w = 0;
            for (int i = 0; i < 4; ++i) w = static_cast<uint16_t>(w << 4 | (pixel(half * 4 + i, y) & 15));
            words.push_back(w);
        }
    return words;
}

inline std::vector<uint8_t> build() {
    Emitter e;
    // Header.
    auto text = [&](uint32_t at, const char* s, size_t width) {
        for (size_t i = 0; i < width; ++i) e.rom[at + i] = static_cast<uint8_t>(i < std::strlen(s) ? s[i] : ' ');
    };
    text(0x100, "SEGA MEGA DRIVE", 16);
    text(0x110, "(C)PLUM 2026.OCT", 16);
    text(0x120, "PLUMSG TEST ROM", 48);
    text(0x150, "PLUMSG TEST ROM", 48);
    text(0x180, "GM 00000000-00", 14);
    text(0x190, "J6", 16);
    text(0x1B0, "", 12);
    text(0x1BC, "", 12);
    text(0x1C8, "", 40);
    text(0x1F0, "JUE", 16);
    e.put32(0x1A4, 0);  // ROM end, patched below
    e.put32(0x1A8, 0xFF0000);
    e.put32(0x1AC, 0xFFFFFF);

    // Code.
    e.label("reset");
    e.w16(0x46FC); e.w16(0x2700);                       // move   #$2700,sr
    e.w16(0x33FC); e.w16(0x0100); e.w32(0xA11100);      // move.w #$100,$A11100  (Z80 bus)
    e.w16(0x33FC); e.w16(0x0100); e.w32(0xA11200);      // move.w #$100,$A11200  (Z80 reset off)
    e.w16(0x43F9); e.w32(0xC00004);                     // lea    $C00004,a1
    e.w16(0x45F9); e.w32(0xC00000);                     // lea    $C00000,a2
    e.w16(0x47F9); e.w32(0xA04000);                     // lea    $A04000,a3
    e.w16(0x41FA); e.rel("table");                      // lea    table(pc),a0
    e.label("regs");
    e.w16(0x3018);                                      // move.w (a0)+,d0
    e.w16(0x6700); e.rel("blocks");                     // beq.w  blocks
    e.w16(0x3280);                                      // move.w d0,(a1)
    e.w16(0x6000); e.rel("regs");                       // bra.w  regs
    e.label("blocks");
    e.w16(0x2018);                                      // move.l (a0)+,d0
    e.w16(0x6700); e.rel("ym");                         // beq.w  ym
    e.w16(0x2280);                                      // move.l d0,(a1)
    e.w16(0x3218);                                      // move.w (a0)+,d1
    e.w16(0x5341);                                      // subq.w #1,d1
    e.label("copy");
    e.w16(0x3498);                                      // move.w (a0)+,(a2)
    e.w16(0x51C9); e.rel("copy");                       // dbra   d1,copy
    e.w16(0x6000); e.rel("blocks");                     // bra.w  blocks
    e.label("ym");
    e.w16(0x7000);                                      // moveq  #0,d0
    e.label("ymloop");
    e.w16(0x1018);                                      // move.b (a0)+,d0
    e.w16(0x6B00); e.rel("main");                       // bmi.w  main
    e.w16(0x1418);                                      // move.b (a0)+,d2
    e.w16(0x1618);                                      // move.b (a0)+,d3
    e.w16(0x1782); e.w16(0x0000);                       // move.b d2,0(a3,d0.w)
    e.w16(0x1783); e.w16(0x0001);                       // move.b d3,1(a3,d0.w)
    e.w16(0x6000); e.rel("ymloop");                     // bra.w  ymloop
    e.label("main");
    e.w16(0x46FC); e.w16(0x2000);                       // move   #$2000,sr
    e.label("idle");
    e.w16(0x6000); e.rel("idle");                       // bra.w  idle
    e.label("vint");
    e.w16(0x5279); e.w32(0xFF0000);                     // addq.w #1,$FF0000
    e.w16(0x23FC); e.w32(0x7C020003); e.w32(0xC00004);  // move.l #VRAM $FC02,$C00004
    e.w16(0x3039); e.w32(0xFF0000);                     // move.w $FF0000,d0
    e.w16(0x33C0); e.w32(0xC00000);                     // move.w d0,$C00000
    e.w16(0x4E73);                                      // rte
    e.label("other");
    e.w16(0x4E73);                                      // rte

    // Table: VDP registers, then data blocks, then YM2612 writes.
    e.label("table");
    for (uint16_t r : std::initializer_list<uint16_t>{0x8004, 0x8164, 0x8230, 0x832C, 0x8407, 0x8578, 0x8700, 0x8AFF,
                                                      0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001, 0x9100, 0x9200})
        e.w16(r);
    e.w16(0);
    auto block = [&](uint32_t command, const std::vector<uint16_t>& words) {
        e.w32(command);
        e.w16(static_cast<uint32_t>(words.size()));
        for (uint16_t w : words) e.w16(w);
    };
    auto vram = [](uint32_t addr) { return 0x40000000u | (addr & 0x3FFF) << 16 | addr >> 14; };

    // Palettes: 0 background, 1 blocks and floor, 2 balls.
    std::vector<uint16_t> cram(64, 0);
    cram[0] = rgb(0, 0, 2);
    cram[1] = rgb(1, 1, 3);
    cram[2] = rgb(1, 2, 4);
    cram[3] = rgb(2, 3, 5);
    cram[17] = rgb(7, 6, 2);
    cram[18] = rgb(6, 3, 0);
    cram[19] = rgb(3, 1, 0);
    cram[20] = rgb(2, 6, 2);
    cram[21] = rgb(1, 4, 1);
    cram[22] = rgb(0, 2, 0);
    cram[33] = rgb(7, 7, 7);
    cram[34] = rgb(7, 2, 6);
    cram[35] = rgb(5, 0, 4);
    cram[36] = rgb(2, 0, 2);
    block(0xC0000000, cram);

    // Tiles: 1 stripes, 2 bevelled block, 3 floor, 4-7 a 16x16 ball (column-major).
    std::vector<uint16_t> tiles;
    auto add = [&](const std::vector<uint16_t>& t) { tiles.insert(tiles.end(), t.begin(), t.end()); };
    add(tile([](int x, int y) {
        const int c = ((x + y) / 2) % 4;
        return c == 3 ? 2 : c + 1;
    }));
    add(tile([](int x, int y) {
        if (y == 0 || x == 0) return 1;
        if (y == 7 || x == 7) return 3;
        return 2;
    }));
    add(tile([](int x, int y) { return y < 2 ? 4 : ((x / 4 + y / 4) % 2 ? 6 : 5); }));
    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 2; ++row)
            add(tile([&](int x, int y) {
                const double px = col * 8 + x + 0.5, py = row * 8 + y + 0.5;
                const double dc = (px - 8) * (px - 8) + (py - 8) * (py - 8);
                if (dc > 7.5 * 7.5) return 0;
                const double dl = (px - 5.5) * (px - 5.5) + (py - 5.5) * (py - 5.5);
                return dl < 6 ? 1 : dl < 30 ? 2 : dl < 80 ? 3 : 4;
            }));
    block(vram(0x20), tiles);

    // Plane B (0xE000): stripes everywhere, low priority.
    block(vram(0xE000), std::vector<uint16_t>(64 * 32, 0x0001));

    // Plane A (0xC000): "PLUMSG" in big blocks (high priority) over a floor.
    static const char* font[6][7] = {
        {"11110", "10001", "10001", "11110", "10000", "10000", "10000"},  // P
        {"10000", "10000", "10000", "10000", "10000", "10000", "11111"},  // L
        {"10001", "10001", "10001", "10001", "10001", "10001", "01110"},  // U
        {"10001", "11011", "10101", "10101", "10001", "10001", "10001"},  // M
        {"01111", "10000", "10000", "01110", "00001", "00001", "11110"},  // S
        {"01110", "10001", "10000", "10111", "10001", "10001", "01111"},  // G
    };
    std::vector<uint16_t> planeA(64 * 32, 0);
    for (int letter = 0; letter < 6; ++letter)
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (font[letter][y][x] == '1') planeA[(5 + y) * 64 + 2 + letter * 6 + x] = 0x8000 | 0x2000 | 2;
    for (int y = 23; y < 28; ++y)
        for (int x = 0; x < 64; ++x) planeA[y * 64 + x] = 0x2000 | 3;
    block(vram(0xC000), planeA);

    // Sprites (0xF000): eight balls along a wave, alternating priority.
    std::vector<uint16_t> sprites;
    for (int i = 0; i < 8; ++i) {
        static const int ys[8] = {60, 84, 100, 120, 150, 170, 150, 110};
        const int x = 24 + i * 36, y = ys[i];
        sprites.push_back(static_cast<uint16_t>(y + 128));
        sprites.push_back(static_cast<uint16_t>(0x0500 | (i < 7 ? i + 1 : 0)));
        sprites.push_back(static_cast<uint16_t>((i % 2 ? 0x8000 : 0) | 0x4000 | 4));
        sprites.push_back(static_cast<uint16_t>(x + 128));
    }
    block(vram(0xF000), sprites);
    block(0x40000010, {0, 0});  // VSRAM
    e.w32(0);

    // YM2612: one channel, algorithm 7 with only operator 4 audible, 440 Hz, key on.
    std::vector<std::array<uint8_t, 3>> ym = {{0, 0x22, 0}, {0, 0x27, 0}, {0, 0x28, 0}, {0, 0x2B, 0}};
    for (uint8_t op : std::initializer_list<uint8_t>{0x00, 0x04, 0x08, 0x0C}) {
        ym.push_back({0, static_cast<uint8_t>(0x30 + op), 0x01});
        ym.push_back({0, static_cast<uint8_t>(0x40 + op), static_cast<uint8_t>(op == 0x0C ? 0x08 : 0x7F)});
        ym.push_back({0, static_cast<uint8_t>(0x50 + op), 0x1F});
        ym.push_back({0, static_cast<uint8_t>(0x60 + op), 0x00});
        ym.push_back({0, static_cast<uint8_t>(0x70 + op), 0x00});
        ym.push_back({0, static_cast<uint8_t>(0x80 + op), 0x0F});
    }
    for (auto w : std::vector<std::array<uint8_t, 3>>{{0, 0xB0, 0x07}, {0, 0xB4, 0xC0}, {0, 0xA4, 0x24}, {0, 0xA0, 0x3A},
                                                      {0, 0x28, 0xF0}})
        ym.push_back(w);
    for (const auto& w : ym)
        for (uint8_t b : w) e.rom.push_back(b);
    e.rom.push_back(0xFF);

    e.link();
    // Vectors.
    e.put32(0, 0x00FFFE00);
    e.put32(4, e.at("reset"));
    for (uint32_t v = 2; v < 64; ++v) e.put32(v * 4, e.at("other"));
    e.put32(0x78, e.at("vint"));

    e.rom.resize((e.rom.size() + 0xFFFF) & ~size_t{0xFFFF}, 0xFF);
    e.put32(0x1A4, static_cast<uint32_t>(e.rom.size() - 1));
    uint16_t sum = 0;
    for (size_t i = 0x200; i + 1 < e.rom.size(); i += 2) sum = static_cast<uint16_t>(sum + (e.rom[i] << 8 | e.rom[i + 1]));
    e.put16(0x18E, sum);
    return e.rom;
}

}  // namespace plumsg::testrom
