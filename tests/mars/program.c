/* The 32X test program for plumsg's core tests (see build.sh). The master runs the
   checks, waits for the slave to run them too and for three VBlank interrupts, writes
   both results to the end of SDRAM, draws a gradient in packed pixel mode and then
   plays a square wave on the PWM. */
#include <stdint.h>
#include <string.h>

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

#include "checks.inc"

#define REG16(a) (*(volatile uint16_t*)(a))
#define SYS 0x20004000
#define COMM(n) REG16(SYS + 0x20 + (n) * 2)
#define FB ((volatile uint16_t*)0x24000000)
#define CRAM ((volatile uint16_t*)(SYS + 0x200))
#define RESULTS ((volatile uint32_t*)0x2603F000)

static volatile int vblanks;

void __attribute__((interrupt_handler)) vint_handler(void) {
    REG16(SYS + 0x16) = 0;  /* VINT clear */
    ++vblanks;
}

void slave_main(void) {
    uint32_t out[16];
    runChecks(out);
    for (int i = 0; i < 16; ++i) RESULTS[16 + i] = out[i];
    COMM(5) = 0x5D;  /* slave done */
    for (;;) {}
}

void master_main(void) {
    uint32_t out[16];
    while (COMM(6) != 1) {}  /* the 68000 says go */
    runChecks(out);
    for (int i = 0; i < 16; ++i) RESULTS[i] = out[i];
    while (COMM(5) != 0x5D) {}

    REG16(SYS) = 0x8008;  /* the VDP for the SH-2s; VINT on */
    __asm__ volatile("ldc %0, sr" : : "r"(0x00));
    while (vblanks < 3) {}
    RESULTS[32] = (uint32_t)vblanks;

    for (int i = 0; i < 256; ++i) CRAM[i] = (uint16_t)((i >> 3) * 0x421);
    for (int y = 0; y < 224; ++y) FB[y] = 0x100 + y * 160;
    for (int y = 0; y < 224; ++y)
        for (int x = 0; x < 160; ++x) FB[0x100 + y * 160 + x] = (uint16_t)(((x * 2) & 0xFF) << 8 | ((x * 2 + 1) & 0xFF));
    REG16(SYS + 0x100) = 1;  /* packed pixels */
    REG16(SYS + 0x10A) = 1;  /* show it */
    COMM(7) = 0xD0;          /* done */

    REG16(SYS + 0x32) = 1047;   /* about 22 kHz */
    REG16(SYS + 0x30) = 0x0105; /* both channels on, timer 1 */
    for (int n = 0;; ++n) {
        while (REG16(SYS + 0x38) & 0x8000) {}
        REG16(SYS + 0x38) = (n / 25) & 1 ? 900 : 150;
    }
}
