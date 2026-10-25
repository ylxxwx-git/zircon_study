/* =========================================================================
 * pmm.h - Zircon-Style Physical Memory Manager (PMM Arena & vm_page_t)
 * References:
 *   - zircon_org/kernel/vm/include/vm/page.h
 *   - zircon_org/kernel/vm/include/vm/pmm.h
 *   - zircon_org/kernel/vm/pmm_arena.cpp
 * ========================================================================= */
#ifndef ABOS_PMM_H
#define ABOS_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE       4096UL
#define PAGE_SHIFT      12
#define PMM_MAX_PAGES   32768U  /* Up to 128MB of 4KB pages */

typedef uint64_t paddr_t;

/* Matches Zircon's enum vm_page_state in zircon_org/kernel/vm/include/vm/page.h */
typedef enum {
    VM_PAGE_STATE_FREE = 0,
    VM_PAGE_STATE_ALLOC,
    VM_PAGE_STATE_OBJECT,   /* Attached to a Zircon VMO */
    VM_PAGE_STATE_WIRED,    /* Kernel wired/reserved memory */
    VM_PAGE_STATE_HEAP,     /* Backing the kernel slab/heap */
    VM_PAGE_STATE_MMU,      /* Page tables (L0-L3) */
    VM_PAGE_STATE_IPC,      /* Channel / MessagePacket buffers */
    VM_PAGE_STATE_COUNT
} vm_page_state_t;

typedef struct vm_page {
    paddr_t  paddr;
    uint32_t next_free_idx; /* Intrusive index in arena free list (0xFFFFFFFF = none) */
    uint8_t  state;         /* vm_page_state_t */
    uint8_t  pin_count;     /* Pin count for DMA / physical VMOs */
    uint16_t flags;
} vm_page_t;

void pmm_init(paddr_t ram_base, uint64_t ram_size, paddr_t reserved_end);
vm_page_t *pmm_alloc_page(vm_page_state_t state, paddr_t *out_paddr);
vm_page_t *pmm_alloc_contiguous(size_t count, vm_page_state_t state, paddr_t *out_paddr);
void pmm_free_page(paddr_t paddr);
vm_page_t *pmm_paddr_to_page(paddr_t paddr);
uint64_t pmm_count_free_pages(void);
uint64_t pmm_count_total_pages(void);
void pmm_dump_state_counts(void);
const char *vm_page_state_name(vm_page_state_t state);

#endif /* ABOS_PMM_H */
