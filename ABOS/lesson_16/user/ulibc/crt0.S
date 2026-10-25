/* =========================================================================
 * user/ulibc/crt0.S - ABOS User-Space C Runtime Startup (crt0)
 *
 * _start: Entry point for standalone EL0 applications (linked at 0x20000000).
 *   1. Receives the bootstrap Channel handle in x0 from SYS_PROCESS_START
 *   2. Initializes standard ulibc handles via __ulibc_init(boot_ch)
 *   3. Invokes int main(void)
 *   4. Passes main()'s return code to sys_process_exit(retcode)
 * ========================================================================= */

.section .text._start, "ax"
.global _start
.type _start, %function
_start:
    /* x0 holds the bootstrap Channel handle (boot_ch) from SYS_PROCESS_START */
    bl __ulibc_init

    /* Call application's int main(void) */
    bl main

    /* Pass return value of main (in x0) to sys_process_exit(retcode) */
    bl sys_process_exit

1:
    wfe
    b 1b
