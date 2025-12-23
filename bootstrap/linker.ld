/* Target the 64-bit ARM architecture */
OUTPUT_ARCH(aarch64)

/* The entry point is the _start label in startup.s */
ENTRY(_start)

/* Define the physical address where our kernel will be loaded (Start of RAM in QEMU 'virt') */
BASE_ADDR = 0x40000000;

SECTIONS
{
    /* The core of our kernel, placed at the start of the RAM */
    . = BASE_ADDR;

    /* Read-only code and data */
    .text : {
        _text_start = .;
        *(.text)
        _text_end = .;
    }

    /* Read/write data */
    .data : {
        _data_start = .;
        *(.data)
        _data_end = .;
    }

    /* Uninitialized data (BSS) - MUST be zeroed by startup.s */
    .bss : {
        _bss_start = .;
        *(.bss)
        _bss_end = .;
    }

    /* Discard any sections we don't need */
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}