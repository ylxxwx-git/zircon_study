/* =========================================================================
 * start.S - ABOS Lesson 04: Physical Trampoline (.idmap) -> Higher-Half VA
 * References:
 *   - zircon_org/kernel/arch/arm64/start.S
 *   - AAOS/lesson_10/kernel/boot_pa.S
 * ========================================================================= */

.equ MAIR_VALUE, 0x000000000000FF00 /* Attr0=Device-nGnRnE, Attr1=Normal-WBWA */
.equ TCR_VALUE,  0x00000000B5213521 /* T0SZ=33, T1SZ=33 (2GB), 4KB granules, ISH WBWA */
.equ PTE_NG_BIT, (1 << 11)          /* Non-Global bit for temporary TTBR0 idmap entries */
.equ PTE_DEVICE, (0x401 | (0 << 2) | (1 << 53) | (1 << 54)) /* 1GB Block, Device, AF, PXN, UXN */
.equ PTE_RAM,    (0x401 | (1 << 2) | (3 << 8))              /* 1GB Block, Normal, AF, ISH */

.section .idmap.text, "ax"
.global _start

_start:
    /* Save DTB physical address in callee-saved x20 on CPU 0 */
    mov     x20, x0

    /* Ensure we are in EL1h */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #2
    b.eq    .Ldrop_el2
    b       .Lin_el1_pa

.Ldrop_el2:
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
    adr     x0, .Lin_el1_pa
    msr     elr_el2, x0
    eret

.Lin_el1_pa:
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF
    cbnz    x9, .Lsecondary_wait_mmu

    /* Primary CPU builds initial 1GB block page tables in .idmap.data */
    adr     x0, idmap_table
    /* idmap_table[0]: VA 0x00000000 -> PA 0x00000000 (Device, Non-Global) */
    ldr     x2, =(PTE_DEVICE | PTE_NG_BIT)
    str     x2, [x0, #0]
    /* idmap_table[1]: VA 0x40000000 -> PA 0x40000000 (RAM, Non-Global) */
    ldr     x1, =0x40000000
    ldr     x2, =(PTE_RAM | PTE_NG_BIT)
    orr     x2, x1, x2
    str     x2, [x0, #8]

    adr     x0, kernel_table
    /* kernel_table[0]: VA 0xFFFFFFFF80000000 -> PA 0x40000000 (Physmap RAM) */
    ldr     x1, =0x40000000
    ldr     x2, =PTE_RAM
    orr     x2, x1, x2
    str     x2, [x0, #0]
    /* kernel_table[1]: VA 0xFFFFFFFFC0000000 -> PA 0x00000000 (Periphmap MMIO) */
    ldr     x2, =PTE_DEVICE
    str     x2, [x0, #8]

    dsb     ish
    adr     x1, mmu_tables_ready
    mov     w2, #1
    str     w2, [x1]
    dsb     ish
    sev
    b       .Lenable_mmu

.Lsecondary_wait_mmu:
    adr     x1, mmu_tables_ready
1:  ldr     w2, [x1]
    cbnz    w2, .Lenable_mmu
    wfe
    b       1b

.Lenable_mmu:
    ldr     x0, =MAIR_VALUE
    msr     mair_el1, x0

    ldr     x0, =TCR_VALUE
    /* Enable 16-bit ASIDs (TCR_EL1.AS = bit 36) if supported by the CPU */
    mrs     x1, id_aa64mmfr0_el1
    ubfx    x1, x1, #4, #4
    cmp     x1, #2
    b.ne    1f
    orr     x0, x0, #(1 << 36)
1:  msr     tcr_el1, x0

    adr     x0, idmap_table
    msr     ttbr0_el1, x0
    adr     x0, kernel_table
    msr     ttbr1_el1, x0
    isb

    tlbi    vmalle1
    ic      iallu
    dsb     ish
    isb

    /* Enable MMU + Data Cache + Instruction Cache (preserving RES1 bits) */
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)   /* SCTLR_EL1.M: MMU enable */
    orr     x0, x0, #(1 << 2)   /* SCTLR_EL1.C: Data/unified cache enable */
    orr     x0, x0, #(1 << 12)  /* SCTLR_EL1.I: Instruction cache enable */
    msr     sctlr_el1, x0
    isb

    /* Branch to Higher-Half virtual address of high_kernel_entry */
    ldr     x0, =high_kernel_entry
    br      x0

.section .idmap.data, "aw"
.balign 4
mmu_tables_ready:
    .word 0

.balign 4096
.global idmap_table
idmap_table:
    .space 4096
.global kernel_table
kernel_table:
    .space 4096

/* =========================================================================
 * Higher-Half Kernel Entry (executes at 0xFFFFFFFF8008xxxx via TTBR1_EL1)
 * ========================================================================= */
.section .text
.global high_kernel_entry
high_kernel_entry:
    /* Install VBAR_EL1 at its Higher-Half Virtual Address */
    ldr     x0, =arm64_el1_exception_base
    msr     vbar_el1, x0
    isb

    /* Set up per-CPU 8KB stack in Higher-Half VA */
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF
    ldr     x10, =_stack_top
    sub     x10, x10, x9, lsl #13
    mov     sp, x10

    cbnz    x9, .Lhigh_secondary

    /* CPU 0: Clear .bss in Higher-Half VA */
    ldr     x10, =_bss_start
    ldr     x11, =_bss_end
.Lbss_loop:
    cmp     x10, x11
    b.ge    .Lbss_done
    stp     xzr, xzr, [x10], #16
    b       .Lbss_loop

.Lbss_done:
    dsb     ish
    mov     x0, x20             /* Pass DTB physical address in x0 */
    bl      lk_main
    b       .Lhalt

.Lhigh_secondary:
    mov     x0, x9
    bl      arm64_secondary_entry

.Lhalt:
    wfe
    b       .Lhalt
