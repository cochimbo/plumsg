#pragma once

// A small Mega Drive program with a sound driver that works like SMPS, for testing the
// song extraction without a commercial ROM: the game asks for sounds by writing a
// number to a queue byte ($FFF00A), which the driver reads and clears in VBlank.
//   $81-$84  songs: a chord every 15 frames on three channels, 32 and round again (8 s)
//   $A0-$AF  sound effects: one note, half a second, on an instrument of their own
//   anything else: silence
// The game asks for $A5 at frame 30, song $81 at frame 60, the effects $A1-$A4 and $A6
// at frames 200-600 and song $82 at frame 900, and sets and clears a byte of its own
// ($FFF030) at frames 100 and 101.
// byCall: the game asks by calling a routine of the driver with the number in d0, as
// Streets of Rage 2 does (the driver, VBlank's code, is then at $8000, away from the
// game's).

#include "test_rom.h"

namespace plumsg::testrom {

inline std::vector<uint8_t> buildMusic(bool byCall = false) {
    Emitter e;
    auto text = [&](uint32_t at, const char* s, size_t width) {
        for (size_t i = 0; i < width; ++i) e.rom[at + i] = static_cast<uint8_t>(i < std::strlen(s) ? s[i] : ' ');
    };
    text(0x100, "SEGA MEGA DRIVE", 16);
    text(0x110, "(C)PLUM 2026.OCT", 16);
    text(0x120, "PLUMSG MUSIC TEST", 48);
    text(0x150, "PLUMSG MUSIC TEST", 48);
    text(0x180, "GM 00000001-00", 14);
    text(0x190, "J", 16);
    text(0x1F0, "JUE", 16);
    e.put32(0x1A8, 0xFF0000);
    e.put32(0x1AC, 0xFFFFFF);

    auto ym = [&](uint8_t reg, uint8_t value) {
        e.w16(0x13FC); e.w16(reg); e.w32(0xA04000);    // move.b #reg,$A04000
        e.w16(0x13FC); e.w16(value); e.w32(0xA04001);  // move.b #value,$A04001
    };
    std::vector<uint32_t> calls;  // where the address of the driver's routine goes
    auto request = [&](uint16_t frame, uint8_t value, uint16_t addr, const std::string& skip) {
        e.w16(0x0C41); e.w16(frame);                   // cmp.w  #frame,d1
        e.w16(0x6600); e.rel(skip);                    // bne.w  skip
        if (byCall && addr == 0xF00A) {
            e.w16(0x303C); e.w16(value);               // move.w #value,d0
            e.w16(0x4EB9); calls.push_back(e.here()); e.w32(0);  // jsr    ask
        } else {
            e.w16(0x11FC); e.w16(value); e.w16(addr);  // move.b #value,(addr).w
        }
        e.label(skip);
    };

    e.label("reset");
    e.w16(0x46FC); e.w16(0x2700);                      // move   #$2700,sr
    e.w16(0x33FC); e.w16(0x0100); e.w32(0xA11100);     // move.w #$100,$A11100  (Z80 bus)
    e.w16(0x33FC); e.w16(0x0100); e.w32(0xA11200);     // move.w #$100,$A11200
    e.w16(0x33FC); e.w16(0x8004); e.w32(0xC00004);     // VDP: mode 1
    e.w16(0x33FC); e.w16(0x8164); e.w32(0xC00004);     // VDP: display and VBlank on
    for (uint8_t op : {0x00, 0x04, 0x08, 0x0C}) {
        ym(static_cast<uint8_t>(0x30 + op), 0x01);
        ym(static_cast<uint8_t>(0x40 + op), op == 0x0C ? 0x08 : 0x7F);
        ym(static_cast<uint8_t>(0x50 + op), 0x1F);
        ym(static_cast<uint8_t>(0x80 + op), 0x0F);
    }
    ym(0xB0, 0x07);
    ym(0xB4, 0xC0);
    e.w16(0x46FC); e.w16(0x2000);                      // move   #$2000,sr
    e.label("main");
    e.w16(0x4E72); e.w16(0x2000);                      // stop   #$2000
    e.w16(0x3238); e.w16(0xF020);                      // move.w ($F020).w,d1
    request(30, 0xA5, 0xF00A, "m0");
    request(60, 0x81, 0xF00A, "m1");
    request(900, 0x82, 0xF00A, "m2");
    request(100, 0x05, 0xF030, "m3");
    request(101, 0x00, 0xF030, "m4");
    request(200, 0xA1, 0xF00A, "m5");
    request(300, 0xA2, 0xF00A, "m6");
    request(400, 0xA3, 0xF00A, "m7");
    request(500, 0xA4, 0xF00A, "m8");
    request(600, 0xA6, 0xF00A, "m9");
    e.w16(0x6000); e.rel("main");                      // bra.w  main

    if (byCall)
        while (e.here() < 0x8000) e.w16(0x4E71);       // nop
    e.label("ask");
    e.w16(0x11C0); e.w16(0xF00A);                      // move.b d0,($F00A).w
    e.w16(0x4E75);                                     // rts

    e.label("vint");
    e.w16(0x48E7); e.w16(0xE000);                      // movem.l d0-d2,-(sp)
    e.w16(0x5278); e.w16(0xF020);                      // addq.w #1,($F020).w
    e.w16(0x1038); e.w16(0xF00A);                      // move.b ($F00A).w,d0
    e.w16(0x6700); e.rel("play");                      // beq.w  play
    e.w16(0x0C00); e.w16(0x0080);                      // cmp.b  #$80,d0
    e.w16(0x6700); e.rel("play");                      // beq.w  play
    e.w16(0x11C0); e.w16(0xF010);                      // move.b d0,($F010).w
    e.w16(0x4278); e.w16(0xF012);                      // clr.w  ($F012).w
    e.w16(0x4238); e.w16(0xF00A);                      // clr.b  ($F00A).w
    ym(0x28, 0x00);
    ym(0x28, 0x01);
    ym(0x28, 0x02);
    e.label("play");
    e.w16(0x1038); e.w16(0xF010);                      // move.b ($F010).w,d0
    e.w16(0x6700); e.rel("done");                      // beq.w  done
    e.w16(0x3238); e.w16(0xF012);                      // move.w ($F012).w,d1
    e.w16(0x5278); e.w16(0xF012);                      // addq.w #1,($F012).w
    e.w16(0x0C00); e.w16(0x00A0);                      // cmp.b  #$A0,d0
    e.w16(0x6500); e.rel("song");                      // bcs.w  song
    e.w16(0x0C00); e.w16(0x00B0);                      // cmp.b  #$B0,d0
    e.w16(0x6400); e.rel("quiet");                     // bcc.w  quiet
    e.w16(0x0C41); e.w16(30);                          // cmp.w  #30,d1
    e.w16(0x6400); e.rel("quiet");                     // bcc.w  quiet
    e.w16(0x4A41);                                     // tst.w  d1
    e.w16(0x6600); e.rel("done");                      // bne.w  done
    ym(0x3C, 0x02);                                    // an instrument of their own
    ym(0xA4, 0x22);
    ym(0xA0, 0x80);
    ym(0x28, 0xF0);
    e.w16(0x6000); e.rel("done");                      // bra.w  done
    e.label("song");
    e.w16(0x0C00); e.w16(0x0085);                      // cmp.b  #$85,d0
    e.w16(0x6400); e.rel("quiet");                     // bcc.w  quiet
    e.w16(0x0C00); e.w16(0x0081);                      // cmp.b  #$81,d0
    e.w16(0x6500); e.rel("quiet");                     // bcs.w  quiet
    e.w16(0x82FC); e.w16(15);                          // divu.w #15,d1
    e.w16(0x4841);                                     // swap   d1
    e.w16(0x4A41);                                     // tst.w  d1
    e.w16(0x6600); e.rel("done");                      // bne.w  done
    e.w16(0x4841);                                     // swap   d1
    e.w16(0x0241); e.w16(31);                          // and.w  #31,d1
    e.w16(0xE749);                                     // lsl.w  #3,d1
    e.w16(0xD200);                                     // add.b  d0,d1
    ym(0x28, 0x00);
    ym(0x28, 0x01);
    ym(0x28, 0x02);
    ym(0x3C, 0x01);
    ym(0xA4, 0x22);
    e.w16(0x13FC); e.w16(0xA0); e.w32(0xA04000);       // move.b #$A0,$A04000
    e.w16(0x13C1); e.w32(0xA04001);                    // move.b d1,$A04001
    ym(0x28, 0xF0);
    ym(0x28, 0xF1);                                    // and two more channels: a song
    ym(0x28, 0xF2);
    e.w16(0x6000); e.rel("done");                      // bra.w  done
    e.label("quiet");
    e.w16(0x4238); e.w16(0xF010);                      // clr.b  ($F010).w
    ym(0x28, 0x00);
    ym(0x28, 0x01);
    ym(0x28, 0x02);
    e.label("done");
    e.w16(0x4CDF); e.w16(0x0007);                      // movem.l (sp)+,d0-d2
    e.w16(0x4E73);                                     // rte
    e.label("other");
    e.w16(0x4E73);                                     // rte

    e.link();
    for (uint32_t at : calls) e.put32(at, e.at("ask"));
    e.put32(0, 0x00FFFE00);
    e.put32(4, e.at("reset"));
    for (uint32_t v = 2; v < 64; ++v) e.put32(v * 4, e.at("other"));
    e.put32(0x78, e.at("vint"));
    e.rom.resize((e.rom.size() + 0xFFFF) & ~size_t{0xFFFF}, 0xFF);
    e.put32(0x1A4, static_cast<uint32_t>(e.rom.size() - 1));
    return e.rom;
}

}  // namespace plumsg::testrom
