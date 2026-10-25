/* =========================================================================
 * linker.ld - ABOS Lesson 02 Linker Script
 * ========================================================================= */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

BASE_ADDR = 0x40080000;

SECTIONS
{
    . = BASE_ADDR;
    __code_start = .;

    .text : ALIGN(4096) {
        _text_start = .;
        KEEP(*(.text.boot))
        *(.text .text.*)
        _text_end = .;
    }

    .rodata : ALIGN(4096) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    }

    .data : ALIGN(4096) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }

    /* Per-CPU early boot stacks: 4 CPUs * 4KB = 16KB */
    . = ALIGN(4096);
    _boot_stacks_bottom = .;
    . += 4096 * 4;
    _stack_top = .;

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
