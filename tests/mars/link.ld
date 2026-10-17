OUTPUT_FORMAT("elf32-sh")
ENTRY(_start)
SECTIONS
{
    . = 0x06000000;
    .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    _end = .;
}
