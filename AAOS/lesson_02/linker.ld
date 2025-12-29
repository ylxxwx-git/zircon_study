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
    /* Code starts at the base of the virtual machine's RAM */
    . = BASE_ADDR;

    /* .text: Read-only code section */
    .text : {
        _text_start = .;
        *(.text.boot)      /* Startup code must be first */
        *(.text)           /* All executable code goes here */
        _text_end = .;
    }

    /* .data: Initialized read/write data section */
    .data : {
        _data_start = .;
        *(.data)
        _data_end = .;
    }

    /* .bss: Uninitialized data section (must be zeroed out by startup.s) */
    .bss : {
        _bss_start = .;
        *(.bss)            /* All uninitialized global variables go here */
        _bss_end = .;
    }

    /* Define the heap start right after BSS */
    _heap_start = .;

    /* Define the stack top at 128MB offset from BASE_ADDR */
    _stack_top = BASE_ADDR + 0x08000000;

    /* Discard unwanted sections to keep the image clean */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}