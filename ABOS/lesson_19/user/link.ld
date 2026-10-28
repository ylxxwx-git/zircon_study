/* =========================================================================
 * user/link.ld - Common Linker Script for All ABOS EL0 User-Space Apps
 *
 * All standalone user-space applications in user/apps/ are linked with:
 *   $(LD) -n -T $(USER_LD) -o <app>.elf $(CRT0_OBJ) <app>.o $(ULIBC_A)
 *
 * Virtual Address Layout:
 *   0x20000000 : _start (defined in user/ulibc/crt0.S, section .text._start)
 *                followed by .text, .rodata, .data, and .bss
 * ========================================================================= */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

USER_APP_BASE = 0x20000000;

SECTIONS
{
    . = USER_APP_BASE;

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
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    .app_end ALIGN(8) : {
        LONG(0x0)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
