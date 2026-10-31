| The Mega CD 32X test program: a stand-in for the Mega CD BIOS that does what a Mega
| CD 32X game's start-up code does once the disc has booted. It turns the 32X on,
| leaves the SH-2s' program in the frame buffer with its header at 0x18, starts the
| SH-2s and says "_CD_"; their boot ROMs copy the program to SDRAM and run it. The
| master's program writes 0x1234 to the fifth communication word. Results go to work
| RAM at 0xFF0000.

        .text
        .org 0
        .long 0x00FFFE00        | stack
        .long start
        .org 0x100
        .ascii "SEGA MEGA DRIVE "

        .org 0x200
start:
        move.w  #0x2700,sr
        cmp.l   #0x4D415253,0xA130EC    | "MARS": there is a 32X
        bne.s   fail
        move.b  #1,0xA15101             | ADEN
        move.w  0x000200,0xFF0004       | the BIOS, not the 32X's vectors, still at 0
        lea     header(pc),a0
        lea     0x840018,a1             | the frame buffer, from 0x18
        move.w  #(program_end-header)/2-1,d0
1:      move.w  (a0)+,(a1)+
        dbra    d0,1b
        move.b  #3,0xA15101             | RES: the SH-2s start
        move.l  #0x5F43445F,0xA15120    | "_CD_"
2:      cmp.l   #0x535F4F4B,0xA15124    | "S_OK": both boot ROMs are done
        bne.s   2b
3:      cmp.w   #0x1234,0xA15128        | the master's program runs
        bne.s   3b
        move.w  #0x600D,0xFF0000
4:      bra.s   4b
fail:   move.w  #0xBAD0,0xFF0000
        bra.s   fail

        .even
header:
        .long   0x06000000              | where the program goes
        .long   program_end-program     | its size
        .long   0x06000000              | the master's entry point
        .long   0x06000010              | the slave's
        .long   0x06000100              | the master's VBR
        .long   0x06000200              | the slave's
        .long   0, 0
program:
        .word   0x9002                  | 00 mov.w @(0x08,pc),r0
        .word   0xC114                  | 02 mov.w r0,@(0x28,gbr)
        .word   0xAFFE                  | 04 bra 0x04
        .word   0x0009                  | 06 nop
        .word   0x1234                  | 08
        .word   0, 0, 0
        .word   0xAFFE                  | 10 bra 0x10 (the slave)
        .word   0x0009                  | 12 nop
program_end:
