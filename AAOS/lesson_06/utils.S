.globl get_core_id

// uint32_t get_core_id(void);
get_core_id:
    mrs x0, mpidr_el1
    and x0, x0, #3 // Extract bits [1:0] for Core ID (assuming cluster 0)
    ret

// void lock_mutex(volatile int *lock);
.globl lock_mutex
lock_mutex:
    mov w1, #1
1:  ldaxr w2, [x0]
    cbnz w2, 2f
    stlxr w2, w1, [x0]
    cbnz w2, 1b
    dmb ish
    ret
2:  wfe
    ldr w2, [x0]
    cbnz w2, 2b
    b 1b

// void unlock_mutex(volatile int *lock);
.globl unlock_mutex
unlock_mutex:
    dmb ish
    stlr wzr, [x0]
    sev
    ret

// MMU Helpers
.global set_ttbr0_el1
set_ttbr0_el1:
    msr ttbr0_el1, x0
    isb
    ret

.global set_ttbr1_el1
set_ttbr1_el1:
    msr ttbr1_el1, x0
    isb
    ret

.global set_tcr_el1
set_tcr_el1:
    msr tcr_el1, x0
    isb
    ret

.global set_mair_el1
set_mair_el1:
    msr mair_el1, x0
    isb
    ret

.global get_sctlr_el1
get_sctlr_el1:
    mrs x0, sctlr_el1
    ret

.global set_sctlr_el1
set_sctlr_el1:
    msr sctlr_el1, x0
    isb
    ret

.global tlb_invalidate_all
tlb_invalidate_all:
    dsb ishst
    tlbi vmalle1is
    dsb ish
    isb
    ret
