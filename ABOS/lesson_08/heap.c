/* =========================================================================
 * heap.c - Kernel Heap Using Higher-Half Physmap Virtual Addresses
 * ========================================================================= */
#include "heap.h"
#include "pmm.h"
#include "mmu.h"
#include "spinlock.h"
#include "debuglog.h"

#define HEAP_INITIAL_PAGES 32U /* 128KB initial kernel heap pool */
#define HEAP_MAGIC         0xAB054EA9U

typedef struct heap_block {
    uint32_t magic;
    uint32_t is_free;
    size_t   size;
    struct heap_block *next;
} __attribute__((aligned(16))) heap_block_t;

static spin_lock_t heap_lock = SPIN_LOCK_INITIAL_VALUE;
static heap_block_t *heap_head = NULL;
static size_t total_heap_bytes = 0;

void heap_init(void) {
    paddr_t base_pa = 0;
    vm_page_t *pages = pmm_alloc_contiguous(HEAP_INITIAL_PAGES, VM_PAGE_STATE_HEAP, &base_pa);
    if (!pages) {
        dprintf(CRITICAL, "[heap] PANIC: failed to allocate pages for kernel heap\n");
        for (;;) __asm__ volatile("wfe");
    }

    total_heap_bytes = HEAP_INITIAL_PAGES * PAGE_SIZE;
    heap_head = (heap_block_t *)paddr_to_physmap(base_pa);
    heap_head->magic   = HEAP_MAGIC;
    heap_head->is_free = 1;
    heap_head->size    = total_heap_bytes - sizeof(heap_block_t);
    heap_head->next    = NULL;

    dprintf(INFO, "[heap] Initialized %u KB kernel heap at VA %p (PA %p)\n",
            (uint32_t)(total_heap_bytes / 1024), (void *)heap_head, (void *)base_pa);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 15UL) & ~15UL;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&heap_lock, &irqstate);

    heap_block_t *curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size >= size + sizeof(heap_block_t) + 32UL) {
                heap_block_t *split = (heap_block_t *)((uint8_t *)(curr + 1) + size);
                split->magic   = HEAP_MAGIC;
                split->is_free = 1;
                split->size    = curr->size - size - sizeof(heap_block_t);
                split->next    = curr->next;

                curr->size = size;
                curr->next = split;
            }
            curr->is_free = 0;
            spin_lock_irqrestore(&heap_lock, irqstate);
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    spin_lock_irqrestore(&heap_lock, irqstate);
    return NULL;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < size; i++) b[i] = 0;
    }
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&heap_lock, &irqstate);

    heap_block_t *blk = ((heap_block_t *)ptr) - 1;
    if (blk->magic != HEAP_MAGIC) {
        spin_lock_irqrestore(&heap_lock, irqstate);
        dprintf(CRITICAL, "[heap] Invalid kfree(%p) - bad magic!\n", ptr);
        return;
    }
    blk->is_free = 1;

    heap_block_t *curr = heap_head;
    while (curr && curr->next) {
        uintptr_t expected_next = (uintptr_t)(curr + 1) + curr->size;
        if (curr->is_free && curr->next->is_free &&
            (uintptr_t)curr->next == expected_next) {
            curr->size += sizeof(heap_block_t) + curr->next->size;
            curr->next  = curr->next->next;
        } else {
            curr = curr->next;
        }
    }

    spin_lock_irqrestore(&heap_lock, irqstate);
}

void heap_dump_stats(void) {
    size_t used_bytes = 0, free_bytes = 0, used_blks = 0, free_blks = 0;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&heap_lock, &irqstate);
    for (heap_block_t *c = heap_head; c != NULL; c = c->next) {
        if (c->is_free) {
            free_bytes += c->size;
            free_blks++;
        } else {
            used_bytes += c->size;
            used_blks++;
        }
    }
    spin_lock_irqrestore(&heap_lock, irqstate);

    dprintf(INFO, "[heap] Stats: used=%lu B (%lu blks), free=%lu B (%lu blks)\n",
            (unsigned long)used_bytes, (unsigned long)used_blks,
            (unsigned long)free_bytes, (unsigned long)free_blks);
}
