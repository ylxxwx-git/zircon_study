/* =================================================================
 * linker.ld - Defines the memory map and section layout.
 * Targets the AArch64 architecture and QEMU 'virt' machine RAM.
 * Defines the _bss_start and _bss_end symbols used in startup.s.
 * ================================================================= */

OUTPUT_ARCH(aarch64)

/* The entry point for execution */
ENTRY(_start)

/* Define the base address of the RAM in the QEMU 'virt' machine */
BASE_ADDR = 0x40000000;
SECTIONS
{
    /* Physical Start of RAM (QEMU loads kernel at 0x40080000) */
    . = 0x40080000;
    
    /* Bootstrap Trampoline (Identity Mapped) */
    .idmap : {
        KEEP(*(.idmap.text))
        KEEP(*(.idmap.data))
        . = ALIGN(4096); /* Align to page boundary */
    }

    /* Move .text to High Address (Physical 0x4008xxxx) */
    . = 0xFFFFFF8040080000 + SIZEOF(.idmap);

    /* Kernel Code */
    .text : AT(ADDR(.idmap) + SIZEOF(.idmap)) {
        _text_start = .;
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        _text_end = .;
    }

    /* Read-only Data */
    .rodata : {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
        _rodata_end = .;
    }

    /* Data (Read-Write) */
    .data : {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4096);
        _data_end = .;
    }

    /* BSS (Zero-initialized) */
    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    }

    /* Define the heap start right after BSS */
    _end = .;
    _heap_start = .;
    
    /* Define the stack top at 128MB offset from BASE_ADDR */
    _stack_top = 0x40000000 + 0x08000000;

    /* Discard unwanted sections to keep the image clean */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}