#include "mutex.h"

void mutex_init(mutex_t *m) {
    m->lock = 0;
}

void mutex_lock(mutex_t *m) {
    unsigned int tmp;
    // Use Load-Exclusive/Store-Exclusive for atomic lock acquisition
    asm volatile(
        "1: ldxr %w0, [%1]\n"      // Load the lock value
        "   cbnz %w0, 1b\n"        // If locked (non-zero), retry
        "   stxr %w0, %w2, [%1]\n" // Try to store 1 (locked)
        "   cbnz %w0, 1b\n"        // If store failed, retry
        "   dmb sy\n"              // Memory barrier
        : "=&r" (tmp)
        : "r" (&m->lock), "r" (1)
        : "memory"
    );
}

void mutex_unlock(mutex_t *m) {
    // Ensure all memory operations are completed before unlocking
    asm volatile("dmb sy" ::: "memory");
    m->lock = 0;
    // Notify other cores (optional for simple spinlock but good practice)
    asm volatile("sev");
}
