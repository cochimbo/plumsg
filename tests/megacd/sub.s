| The sub CPU side of the Mega CD test program, loaded into program RAM at 0.
| It answers the console's 68000, then tries the timer, the CD drive and its
| controller, the graphics chip, the PCM chip and CD audio, leaving what it saw in
| program RAM at RESULTS (one word each) for the test to check.

        .equ GA, 0xFF8000       | gate array registers
        .equ RESULTS, 0x7F000
        .equ R_HANDSHAKE, RESULTS+0
        .equ R_TIMER, RESULTS+2         | timer interrupts taken
        .equ R_EXTERNAL, RESULTS+4      | level 2 interrupts from the 68000
        .equ R_TRACKS, RESULTS+6        | first and last track numbers from the drive
        .equ R_DATA0, RESULTS+8         | first two words of sector 0, read by the CPU
        .equ R_DATA1, RESULTS+10
        .equ R_EDT, RESULTS+12          | end of transfer flag after reading
        .equ R_DMA, RESULTS+14          | first word of sector 0 after DMA to word RAM
        .equ R_GPU, RESULTS+16          | a word the graphics chip drew
        .equ R_BRAM, RESULTS+18         | a byte read back from backup RAM
        .equ R_DECODED, RESULTS+20      | sectors the controller decoded
        .equ R_GPUIRQ, RESULTS+22       | graphics interrupts taken
        .equ V_POINTER, RESULTS+0x40    | block pointer of sector 0 in the controller's RAM
        .equ V_FOUND, RESULTS+0x42

        .text
        .org 0
        .long 0x0007FFF0        | stack
        .long start
        .org 0x64
        .long gpu_irq           | level 1: graphics
        .long external_irq      | level 2: from the 68000
        .long timer_irq         | level 3: timer
        .long cdd_irq           | level 4: drive
        .long cdc_irq           | level 5: controller
        .long rte_only          | level 6: subcode

        .org 0x100
start:
        move.w  #0x2700,sr
        lea     GA,a6
        | Interrupts: graphics, 68000, timer, drive and controller.
        move.w  #0x3E,0x32(a6)
        move.w  #0x2000,sr
        move.w  #0x1234,0x20(a6)        | alive
1:      move.w  0x10(a6),d0             | wait for the 68000's word
        cmp.w   #0xBEEF,d0
        bne.s   1b
        addq.w  #1,d0
        move.w  d0,0x22(a6)
        move.w  #1,R_HANDSHAKE

        | Timer: an interrupt every 10 ticks of 30.72 us.
        move.w  #10,0x30(a6)
2:      cmp.w   #3,R_TIMER
        blt.s   2b
        move.w  #0,0x30(a6)

        | Backup RAM: odd bytes at 0xFE0000.
        move.b  #0x5A,0xFE0101
        moveq   #0,d0
        move.b  0xFE0101,d0
        move.w  d0,R_BRAM

        | The drive: its clock on, then ask for the first and last tracks.
        move.b  #4,0x37(a6)
        lea     cmd_tracks(pc),a0
        bsr     cdd_command
        moveq   #0,d0
        move.b  0x3A(a6),d0             | status 2-5: first track tens, units, last tens, units
        lsl.w   #4,d0
        or.b    0x3B(a6),d0
        lsl.w   #4,d0
        or.b    0x3C(a6),d0
        lsl.w   #4,d0
        or.b    0x3D(a6),d0
        move.w  d0,R_TRACKS

        | The controller: decoder on, writing sectors to its RAM; interrupt when one is in.
        move.b  #0x01,0x05(a6)          | IFCTRL
        move.b  #0x22,0x07(a6)          | decoder interrupt, data out
        move.b  #0x0A,0x05(a6)          | CTRL0
        move.b  #0x84,0x07(a6)          | decoder on, write to RAM
        move.b  #0x00,0x07(a6)          | CTRL1
        lea     cmd_play_start(pc),a0   | read from 00:02:00
        bsr     cdd_command
3:      tst.w   V_FOUND
        beq.s   3b
        move.b  #0x0A,0x05(a6)
        move.b  #0x00,0x07(a6)          | decoder off

        | Read sector 0's data with the CPU.
        move.b  #3,0x04(a6)             | destination: sub CPU
        bsr     start_transfer
4:      btst    #6,0x04(a6)             | data ready
        beq.s   4b
        move.w  0x08(a6),R_DATA0
        move.w  0x08(a6),R_DATA1
        move.w  #1021,d1
5:      move.w  0x08(a6),d0
        dbra    d1,5b
        move.w  0x04(a6),d0
        and.w   #0x8000,d0
        move.w  d0,R_EDT

        | And by DMA into word RAM, once the 68000 has handed it over.
6:      btst    #1,0x03(a6)
        beq.s   6b
        move.b  #7,0x04(a6)             | destination: word RAM
        move.w  #0,0x0A(a6)
        bsr     start_transfer
7:      btst    #7,0x04(a6)             | end of transfer
        beq.s   7b
        move.w  0x080000,R_DMA

        | Graphics: stamp 1 (16x16, colour 5) at the top left of a 1x1 map, drawn
        | onto 8 dots of one line of an image at word RAM 0x10000.
        lea     0x080080,a0             | stamp 1
        move.w  #63,d1
8:      move.w  #0x5555,(a0)+
        dbra    d1,8b
        move.w  #1,0x0A0000             | map entry (0,0) at 0x20000
        lea     0x0B0000,a0             | vector table at 0x30000
        move.w  #0,(a0)+                | x
        move.w  #0,(a0)+                | y
        move.w  #0x0800,(a0)+           | dx: one pixel
        move.w  #0,(a0)+                | dy
        move.w  #0,0x58(a6)             | 16x16 stamps, 1x1 map, no repeat
        move.w  #0x8000,0x5A(a6)        | map base 0x20000
        move.w  #0,0x5C(a6)             | one cell high
        move.w  #0x4000,0x5E(a6)        | image base 0x10000
        move.w  #0,0x60(a6)
        move.w  #8,0x62(a6)             | 8 dots
        move.w  #1,0x64(a6)             | 1 line
        move.w  #0xC000,0x66(a6)        | vector table 0x30000, and go
9:      tst.w   0x58(a6)
        bmi.s   9b
        move.w  0x090000,R_GPU
        move.b  #1,0x03(a6)             | word RAM back to the 68000

        | PCM: channel 0 plays a square wave from wave RAM bank 0.
        lea     0xFF0000,a0
        move.b  #0x40,0x0F(a0)          | CTRL: select channel 0
        move.b  #0xFF,0x01(a0)          | envelope
        move.b  #0xFF,0x03(a0)          | pan
        move.b  #0x00,0x05(a0)          | step 1.0
        move.b  #0x08,0x07(a0)
        move.b  #0x00,0x09(a0)          | loop at 0
        move.b  #0x00,0x0B(a0)
        move.b  #0x00,0x0D(a0)          | start at 0
        move.b  #0x00,0x0F(a0)          | CTRL: bank 0
        lea     0xFF2001,a1
        move.w  #127,d1
10:     move.b  #0xF0,(a1)
        move.b  #0x70,2(a1)
        addq.l  #4,a1
        dbra    d1,10b
        move.b  #0xFF,(a1)              | loop marker
        move.b  #0x80,0x0F(a0)          | CTRL: sound on
        move.b  #0xFE,0x11(a0)          | channel 0 on
        move.w  #0x9C,0x2C(a6)          | status 6: PCM playing
        move.w  #2,0x30(a6)             | timer again, to wait a while
        move.w  #0,R_TIMER
11:     cmp.w   #3000,R_TIMER
        blt.s   11b
        move.w  #0,0x30(a6)
        move.b  #0xFF,0x11(a0)          | channel off
        move.w  #0,0x2C(a6)

        | CD audio: where does track 2 start? Then play from there.
        lea     cmd_track2(pc),a0
        bsr     cdd_command
        lea     cmd_play(pc),a0
        move.b  0x3A(a6),2(a0)          | minutes, seconds and frames from status 2-7
        move.b  0x3B(a6),3(a0)
        move.b  0x3C(a6),4(a0)
        move.b  0x3D(a6),5(a0)
        move.b  0x3E(a6),d0
        and.b   #7,d0                   | without the data track flag
        move.b  d0,6(a0)
        move.b  0x3F(a6),7(a0)
        bsr     cdd_command
        move.w  #0xD0,0x2E(a6)          | status 7: done
12:     bra.s   12b

| Sends the drive the command at a0 (9 nibbles, one per byte), with its checksum.
cdd_command:
        moveq   #0,d0
        moveq   #8,d1
        lea     0x42(a6),a1
1:      move.b  (a0),d2
        add.b   d2,d0
        move.b  (a0)+,(a1)+
        dbra    d1,1b
        not.b   d0
        and.b   #0x0F,d0
        move.b  d0,(a1)                 | the last nibble sends it
        rts

| Starts a 2048-byte transfer of sector 0's data from the controller's RAM.
start_transfer:
        move.b  #0x02,0x05(a6)          | DBCL, DBCH, DACL, DACH, DTRG in a row
        move.b  #0xFF,0x07(a6)
        move.b  #0x07,0x07(a6)
        move.w  V_POINTER,d0
        addq.w  #4,d0                   | past the header
        move.b  d0,0x07(a6)
        lsr.w   #8,d0
        move.b  d0,0x07(a6)
        move.b  #0,0x07(a6)             | go
        rts

cdc_irq:
        movem.l d0-d1,-(sp)
        addq.w  #1,R_DECODED
        move.b  #0x04,0x05(a6)          | HEAD0-3, then PTL and PTH
        move.b  0x07(a6),d0             | minutes
        move.b  0x07(a6),d0             | seconds
        lsl.w   #8,d0
        move.b  0x07(a6),d0             | frames
        move.b  0x07(a6),d1             | mode
        cmp.w   #0x0200,d0              | 00:02:00 is sector 0
        bne.s   1f
        tst.w   V_FOUND
        bne.s   1f
        move.b  0x07(a6),d1             | PTL
        move.b  0x07(a6),d0             | PTH
        lsl.w   #8,d0
        move.b  d1,d0
        move.w  d0,V_POINTER
        move.w  #1,V_FOUND
1:      move.b  #0x0F,0x05(a6)          | STAT3: acknowledges the decoder
        move.b  0x07(a6),d0
        movem.l (sp)+,d0-d1
        rte

timer_irq:
        addq.w  #1,R_TIMER
        rte
external_irq:
        addq.w  #1,R_EXTERNAL
        rte
gpu_irq:
        addq.w  #1,R_GPUIRQ
        rte
cdd_irq:
rte_only:
        rte

cmd_tracks:     .byte 2,0,0,4,0,0,0,0,0
cmd_play_start: .byte 3,0,0,0,0,2,0,0,0
cmd_track2:     .byte 2,0,0,5,0,2,0,0,0
cmd_play:       .byte 3,0,0,0,0,0,0,0,0
        .even
