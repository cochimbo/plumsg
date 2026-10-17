#pragma once

// A second tiny program, for checking that input reaches the game: it reads pad 1
// over and over (both halves of the TH multiplex), keeps the two bytes it read at
// 0xFF0000 and 0xFF0001 and paints the backdrop with a colour made of the pressed
// buttons (up, down and left are shades of red, right, B and C of green, A and Start
// of blue). With nothing pressed the screen is black.

#include <cstdint>
#include <vector>

#include "test_rom.h"

namespace plumsg::testrom {

inline std::vector<uint8_t> buildPadTest() {
    Emitter e;
    auto text = [&](uint32_t at, const char* s, size_t width) {
        for (size_t i = 0; i < width; ++i) e.rom[at + i] = static_cast<uint8_t>(i < std::strlen(s) ? s[i] : ' ');
    };
    text(0x100, "SEGA MEGA DRIVE", 16);
    text(0x110, "(C)PLUM 2026.OCT", 16);
    text(0x120, "PLUMSG PAD TEST", 48);
    text(0x150, "PLUMSG PAD TEST", 48);
    text(0x180, "GM 00000001-00", 14);
    text(0x190, "J", 16);
    text(0x1F0, "JUE", 16);
    e.put32(0x1A8, 0xFF0000);
    e.put32(0x1AC, 0xFFFFFF);

    e.label("reset");
    e.w16(0x46FC); e.w16(0x2700);                       // move   #$2700,sr
    e.w16(0x43F9); e.w32(0xC00004);                     // lea    $C00004,a1
    e.w16(0x45F9); e.w32(0xC00000);                     // lea    $C00000,a2
    e.w16(0x32BC); e.w16(0x8004);                       // move.w #$8004,(a1)
    e.w16(0x32BC); e.w16(0x8144);                       // move.w #$8144,(a1)  display on
    e.w16(0x32BC); e.w16(0x8700);                       // move.w #$8700,(a1)  backdrop = colour 0
    e.w16(0x32BC); e.w16(0x8C81);                       // move.w #$8C81,(a1)  H40
    e.w16(0x13FC); e.w16(0x0040); e.w32(0xA10009);      // move.b #$40,$A10009 TH is an output
    e.label("loop");
    e.w16(0x13FC); e.w16(0x0040); e.w32(0xA10003);      // move.b #$40,$A10003 TH high
    e.w16(0x4E71); e.w16(0x4E71);                       // nop; nop
    e.w16(0x1039); e.w32(0xA10003);                     // move.b $A10003,d0   ..CBRLDU
    e.w16(0x13FC); e.w16(0x0000); e.w32(0xA10003);      // move.b #$00,$A10003 TH low
    e.w16(0x4E71); e.w16(0x4E71);                       // nop; nop
    e.w16(0x1239); e.w32(0xA10003);                     // move.b $A10003,d1   ..SA00DU
    e.w16(0x13C0); e.w32(0xFF0000);                     // move.b d0,$FF0000
    e.w16(0x13C1); e.w32(0xFF0001);                     // move.b d1,$FF0001
    e.w16(0x4600);                                      // not.b  d0
    e.w16(0x0240); e.w16(0x003F);                       // andi.w #$3F,d0
    e.w16(0xD040);                                      // add.w  d0,d0
    e.w16(0x4601);                                      // not.b  d1
    e.w16(0x0241); e.w16(0x0030);                       // andi.w #$30,d1
    e.w16(0xEB49);                                      // lsl.w  #5,d1
    e.w16(0x8041);                                      // or.w   d1,d0
    e.w16(0x22BC); e.w32(0xC0000000);                   // move.l #$C0000000,(a1) CRAM 0
    e.w16(0x3480);                                      // move.w d0,(a2)
    e.w16(0x6000); e.rel("loop");                       // bra.w  loop
    e.link();

    e.put32(0, 0x00FFFE00);
    e.put32(4, e.at("reset"));
    while (e.rom.size() % 0x400) e.rom.push_back(0xFF);
    e.put32(0x1A4, static_cast<uint32_t>(e.rom.size() - 1));
    return e.rom;
}

// A third one, for netplay: it reads both pads over and over and folds every read
// into a running value (rotate, then xor each pad's byte in), kept at 0xFF0004 and
// painted on the backdrop as it changes, line by line. So each frame's picture
// depends on everything both players pressed before it: a pad that reached the
// console on the wrong frame changes every frame after it.
inline std::vector<uint8_t> buildTwoPadTest() {
    Emitter e;
    auto text = [&](uint32_t at, const char* s, size_t width) {
        for (size_t i = 0; i < width; ++i) e.rom[at + i] = static_cast<uint8_t>(i < std::strlen(s) ? s[i] : ' ');
    };
    text(0x100, "SEGA MEGA DRIVE", 16);
    text(0x110, "(C)PLUM 2026.OCT", 16);
    text(0x120, "PLUMSG TWO PAD TEST", 48);
    text(0x150, "PLUMSG TWO PAD TEST", 48);
    text(0x180, "GM 00000002-00", 14);
    text(0x190, "J", 16);
    text(0x1F0, "JUE", 16);
    e.put32(0x1A8, 0xFF0000);
    e.put32(0x1AC, 0xFFFFFF);

    e.label("reset");
    e.w16(0x46FC); e.w16(0x2700);                       // move   #$2700,sr
    e.w16(0x43F9); e.w32(0xC00004);                     // lea    $C00004,a1
    e.w16(0x45F9); e.w32(0xC00000);                     // lea    $C00000,a2
    e.w16(0x32BC); e.w16(0x8004);                       // move.w #$8004,(a1)
    e.w16(0x32BC); e.w16(0x8144);                       // move.w #$8144,(a1)  display on
    e.w16(0x32BC); e.w16(0x8700);                       // move.w #$8700,(a1)  backdrop = colour 0
    e.w16(0x32BC); e.w16(0x8C81);                       // move.w #$8C81,(a1)  H40
    e.w16(0x13FC); e.w16(0x0040); e.w32(0xA10009);      // move.b #$40,$A10009 TH is an output (pad 1)
    e.w16(0x13FC); e.w16(0x0040); e.w32(0xA1000B);      // move.b #$40,$A1000B and on pad 2
    e.w16(0x7E00);                                      // moveq  #0,d7
    e.label("loop");
    e.w16(0x13FC); e.w16(0x0040); e.w32(0xA10003);      // move.b #$40,$A10003 TH high
    e.w16(0x4E71); e.w16(0x4E71);                       // nop; nop
    e.w16(0x1039); e.w32(0xA10003);                     // move.b $A10003,d0   ..CBRLDU
    e.w16(0x13FC); e.w16(0x0000); e.w32(0xA10003);      // move.b #$00,$A10003 TH low
    e.w16(0x4E71); e.w16(0x4E71);                       // nop; nop
    e.w16(0x1239); e.w32(0xA10003);                     // move.b $A10003,d1   ..SA00DU
    e.w16(0x13FC); e.w16(0x0040); e.w32(0xA10005);      // move.b #$40,$A10005 pad 2, TH high
    e.w16(0x4E71); e.w16(0x4E71);                       // nop; nop
    e.w16(0x1439); e.w32(0xA10005);                     // move.b $A10005,d2
    e.w16(0x13FC); e.w16(0x0000); e.w32(0xA10005);      // move.b #$00,$A10005 TH low
    e.w16(0x4E71); e.w16(0x4E71);                       // nop; nop
    e.w16(0x1639); e.w32(0xA10005);                     // move.b $A10005,d3
    e.w16(0x13C0); e.w32(0xFF0000);                     // move.b d0,$FF0000
    e.w16(0x13C1); e.w32(0xFF0001);                     // move.b d1,$FF0001
    e.w16(0x13C2); e.w32(0xFF0002);                     // move.b d2,$FF0002
    e.w16(0x13C3); e.w32(0xFF0003);                     // move.b d3,$FF0003
    e.w16(0xE35F);                                      // rol.w  #1,d7
    e.w16(0xB107);                                      // eor.b  d0,d7
    e.w16(0xE35F);                                      // rol.w  #1,d7
    e.w16(0xB307);                                      // eor.b  d1,d7
    e.w16(0xE35F);                                      // rol.w  #1,d7
    e.w16(0xB507);                                      // eor.b  d2,d7
    e.w16(0xE35F);                                      // rol.w  #1,d7
    e.w16(0xB707);                                      // eor.b  d3,d7
    e.w16(0x33C7); e.w32(0xFF0004);                     // move.w d7,$FF0004
    e.w16(0x3807);                                      // move.w d7,d4
    e.w16(0x0244); e.w16(0x0EEE);                       // andi.w #$0EEE,d4
    e.w16(0x22BC); e.w32(0xC0000000);                   // move.l #$C0000000,(a1) CRAM 0
    e.w16(0x3484);                                      // move.w d4,(a2)
    e.w16(0x6000); e.rel("loop");                       // bra.w  loop
    e.link();

    e.put32(0, 0x00FFFE00);
    e.put32(4, e.at("reset"));
    while (e.rom.size() % 0x400) e.rom.push_back(0xFF);
    e.put32(0x1A4, static_cast<uint32_t>(e.rom.size() - 1));
    return e.rom;
}

}  // namespace plumsg::testrom
