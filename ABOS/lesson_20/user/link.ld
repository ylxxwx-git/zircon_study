/* =========================================================================
 * user/link.ld - Common Linker Script for All ABOS EL0 User-Space Executables
 *
 * Used by all standalone EL0 user-space servers, shells, and applications:
 *   - user/drivers/console_srv.c -> image/console-srv.bin
 *   - user/drivers/fs_srv.c      -> image/fs-srv.bin
 *   - user/shell/shell.c         -> image/shell.bin
 *   - user/apps/*.c              -> image/<app>.bin
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
