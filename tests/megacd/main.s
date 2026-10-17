| The console's side of the Mega CD test program: a stand-in for the BIOS, which
| loads the sub CPU's program into program RAM, starts it and talks to it.
| Results go to work RAM at 0xFF0000.

        .equ GA, 0xA12000

        .text
        .org 0
        .long 0x00FFFE00        | stack
        .long start
        .org 0x100
        .ascii "SEGA MEGA DRIVE "

        .org 0x200
start:
        move.w  #0x2700,sr
        lea     GA,a5
        move.w  #0x0002,(a5)            | sub CPU held in reset; its bus is ours
        lea     sub_program(pc),a0
        lea     0x020000,a1             | program RAM, bank 0
        move.w  #(sub_end-sub_program)/2-1,d0
1:      move.w  (a0)+,(a1)+
        dbra    d0,1b
        move.w  #0x0001,(a5)            | let it run
2:      cmp.w   #0x1234,0x20(a5)
        bne.s   2b
        move.w  #0xBEEF,0x10(a5)
3:      cmp.w   #0xBEF0,0x22(a5)
        bne.s   3b
        move.b  #1,(a5)                 | level 2 interrupt for the sub CPU
        move.b  #2,0x03(a5)             | word RAM to the sub CPU
4:      cmp.w   #0xD0,0x2E(a5)
        bne.s   4b
5:      btst    #0,0x03(a5)             | word RAM back?
        beq.s   5b
        move.w  0x210000,0xFF0002       | what the graphics chip drew
        move.w  #0x600D,0xFF0000
6:      bra.s   6b

        .even
sub_program:
        .incbin "sub.bin"
sub_end:
