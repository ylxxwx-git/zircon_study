OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x20000000;

    .text : {
        KEEP(*(.text._start))
        *(.text .text.*)
    }

    .rodata ALIGN(8) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(8) : {
        *(.data .data.*)
    }

    .bss ALIGN(8) : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
