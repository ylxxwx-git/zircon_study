/* =========================================================================
 * start.S - AArch64 Early Boot, EL3/EL2 -> EL1 & VBAR_EL1 Installation
 * ========================================================================= */

.section .text.boot
.global _start
.global el1_entry

_start:
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF

    cbnz    x9, .Lcheck_el

    ldr     x10, =boot_dtb_paddr
    str     x0, [x10]

    mrs     x11, CurrentEL
    lsr     x11, x11, #2
    and     x11, x11, #3
    ldr     x10, =arch_boot_el
    str     x11, [x10]

.Lcheck_el:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3

    cmp     x0, #3
    b.eq    .Lel3_entry

    cmp     x0, #2
    b.eq    .Lel2_to_el1

    b       el1_entry

.Lel3_entry:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, .Lel3_secondary_park

    ldr     x0, =el3_vector_table
    msr     vbar_el3, x0

    mov     x0, #(1 << 10)
    orr     x0, x0, #(1 << 8)
    orr     x0, x0, #(1 << 0)
    msr     scr_el3, x0

    mov     x0, #0x3C9
    msr     spsr_el3, x0
    adr     x0, .Lel2_to_el1
    msr     elr_el3, x0
    eret

.Lel3_secondary_park:
    ldr     x1, =psci_mailbox
    lsl     x2, x0, #3
    add     x1, x1, x2
.Lwait_mailbox:
    wfe
    ldr     x2, [x1]
    cbz     x2, .Lwait_mailbox

    ldr     x0, =el3_vector_table
    msr     vbar_el3, x0

    mov     x0, #(1 << 10)
    orr     x0, x0, #(1 << 8)
    orr     x0, x0, #(1 << 0)
    msr     scr_el3, x0

    mov     x0, #0x3C9
    msr     spsr_el3, x0
    msr     elr_el3, x2
    eret

.Lel2_to_el1:
    ldr     x0, =el2_vector_table
    msr     vbar_el2, x0

    mrs     x0, cnthctl_el2
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    mov     x0, #0x33FF
    msr     cptr_el2, x0

    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    mov     x0, #0x3C5
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    /* Enable FPU/SIMD in EL1 */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0

    /* Install Zircon-style EL1 exception vector table (VBAR_EL1) on all CPUs */
    ldr     x0, =arm64_el1_exception_base
    msr     vbar_el1, x0
    isb

    /* Set up per-CPU 4KB boot stack */
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF
    ldr     x10, =_stack_top
    sub     x10, x10, x9, lsl #12
    mov     sp, x10

    cbnz    x9, .Lsecondary_cpu

    /* Primary CPU zeroes .bss */
    ldr     x10, =_bss_start
    ldr     x11, =_bss_end
.Lbss_loop:
    cmp     x10, x11
    b.ge    .Lbss_done
    stp     xzr, xzr, [x10], #16
    b       .Lbss_loop

.Lbss_done:
    dsb     ish
    bl      lk_main
    b       .Lhalt

.Lsecondary_cpu:
    mov     x0, x9
    bl      arm64_secondary_entry

.Lhalt:
    wfe
    b       .Lhalt

.align 11
el2_vector_table:
    .rept 8
    .align 7; b .
    .endr
    .align 7; b el2_sync_handler
    .rept 7
    .align 7; b .
    .endr

el2_sync_handler:
    msr     tpidr_el2, x30
    mrs     x30, esr_el2
    lsr     x30, x30, #26
    cmp     x30, #0x16
    b.ne    .
    smc     #0
    mrs     x30, tpidr_el2
    eret

.align 11
el3_vector_table:
    .rept 8
    .align 7; b .
    .endr
    .align 7; b el3_sync_handler
    .rept 7
    .align 7; b .
    .endr

el3_sync_handler:
    mrs     x30, esr_el3
    lsr     x30, x30, #26
    cmp     x30, #0x17
    b.ne    .

    ldr     x30, =0xC4000003
    cmp     x0, x30
    b.ne    .

    ldr     x30, =psci_mailbox
    and     x1, x1, #0xFF
    lsl     x1, x1, #3
    add     x30, x30, x1
    str     x2, [x30]
    dsb     sy
    sev
    mov     x0, #0
    eret

.section .data
.balign 8
.global boot_dtb_paddr
boot_dtb_paddr:
    .quad 0

.global arch_boot_el
arch_boot_el:
    .quad 0

psci_mailbox:
    .skip 8 * 4
