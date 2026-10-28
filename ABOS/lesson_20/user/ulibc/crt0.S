/* =========================================================================
 * user/ulibc/crt0.S - ABOS User-Space C Runtime Startup (crt0)
 *
 * _start: Entry point for all standalone EL0 servers, shells, and apps
 *         linked with user/link.ld at VA 0x20000000.
 *   1. Receives (boot_ch, arg1) in (x0, x1) from SYS_PROCESS_START
 *   2. Zeroes the .bss section ([__bss_start .. __bss_end))
 *   3. Initializes startup handles via __ulibc_init(boot_ch, arg1)
 *   4. Invokes int main(void)
 *   5. Passes main()'s return code to sys_process_exit(retcode)
 * ========================================================================= */

.section .text._start, "ax"
.global _start
.type _start, %function
_start:
    /* Preserve startup arguments (x0 = boot_ch, x1 = arg1) */
    mov     x19, x0
    mov     x20, x1

    /* Zero .bss ([__bss_start .. __bss_end)) */
    ldr     x2, =__bss_start
    ldr     x3, =__bss_end
1:
    cmp     x2, x3
    b.hs    2f
    str     xzr, [x2], #8
    b       1b
2:

    /* Initialize ulibc startup environment: __ulibc_init(boot_ch, arg1) */
    mov     x0, x19
    mov     x1, x20
    bl      __ulibc_init

    /* Call application/server int main(void) */
    bl      main

    /* Pass return value of main (in x0) to sys_process_exit(retcode) */
    bl      sys_process_exit

3:
    wfe
    b       3b
