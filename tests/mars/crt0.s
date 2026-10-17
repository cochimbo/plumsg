! Start-up of the 32X test program, loaded at 0x06000000 (the SDRAM): the exception
! vectors (VBR points here), then the master's entry at 0x200 and the slave's at 0x220.
    .section .vectors, "ax"
    .global _start
_start:
    .rept 70
    .long _hang
    .endr
    .long _vint_handler      ! 70: VINT
    .rept 57
    .long _hang
    .endr

    .org 0x200
_master_entry:
    mov.l   master_stack, r15
    mov.l   master_main, r0
    jsr     @r0
    nop
_hang:
    bra     _hang
    nop
    .align 2
master_stack: .long 0x06040000
master_main:  .long _master_main

    .org 0x220
_slave_entry:
    mov.l   slave_stack, r15
    mov.l   slave_main, r0
    jsr     @r0
    nop
1:  bra     1b
    nop
    .align 2
slave_stack: .long 0x0603F800
slave_main:  .long _slave_main
