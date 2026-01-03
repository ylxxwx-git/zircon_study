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
    cbnz w2, 1b
    stlxr w2, w1, [x0]
    cbnz w2, 1b
    ret

// void unlock_mutex(volatile int *lock);
.globl unlock_mutex
unlock_mutex:
    stlr wzr, [x0]
    ret
