/* =========================================================================
 * linker.ld - ABOS Lesson 04+ Higher-Half Kernel Linker Script
 * Physical load address: 0x40080000
 * Kernel virtual base  : 0xFFFFFFFF80080000 (PA + 0xFFFFFFFF40000000)
 * ========================================================================= */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

BASE_PA       = 0x40000000;
KERNEL_VA_OFF = 0xFFFFFFFF40000000;

SECTIONS
{
    /* Physical entry trampoline (.idmap) loaded at 0x40080000 */
    . = 0x40080000;

    .idmap : {
        KEEP(*(.idmap.text))
        KEEP(*(.idmap.data))
        . = ALIGN(4096);
    }

    /* Advance location counter to Higher-Half Virtual Address (TTBR1_EL1) */
    . = KERNEL_VA_OFF + 0x40080000 + SIZEOF(.idmap);
    __code_start = .;

    .text : AT(ADDR(.idmap) + SIZEOF(.idmap)) {
        _text_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        _text_end = .;
    }

    .rodata : {
        _rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4096);
        _rodata_end = .;
    }

    .data : {
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4096);
        _data_end = .;
    }

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    }

    /* Per-CPU 8KB kernel stacks in Higher-Half VA */
    . = ALIGN(4096);
    _boot_stacks_bottom = .;
    . += 8192 * 4;
    _stack_top = .;

    . = ALIGN(4096);
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
