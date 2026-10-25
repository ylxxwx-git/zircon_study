/* =========================================================================
 * pmm.h - Zircon-Style Physical Memory Manager (Higher-Half Physmap)
 * ========================================================================= */
#ifndef ABOS_PMM_H
#define ABOS_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE       4096UL
#define PAGE_SHIFT      12
#define PMM_MAX_PAGES   32768U

typedef uint64_t paddr_t;

typedef enum {
    VM_PAGE_STATE_FREE = 0,
    VM_PAGE_STATE_ALLOC,
    VM_PAGE_STATE_OBJECT,
    VM_PAGE_STATE_WIRED,
    VM_PAGE_STATE_HEAP,
    VM_PAGE_STATE_MMU,
    VM_PAGE_STATE_IPC,
    VM_PAGE_STATE_COUNT
} vm_page_state_t;

typedef struct vm_page {
    paddr_t  paddr;
    uint32_t next_free_idx;
    uint8_t  state;
    uint8_t  pin_count;
    uint16_t flags;
} vm_page_t;

void pmm_init(paddr_t ram_base, uint64_t ram_size, paddr_t kernel_end_pa,
              paddr_t dtb_pa, uint32_t dtb_size);
vm_page_t *pmm_alloc_page(vm_page_state_t state, paddr_t *out_paddr);
vm_page_t *pmm_alloc_contiguous(size_t count, vm_page_state_t state, paddr_t *out_paddr);
void pmm_free_page(paddr_t paddr);
vm_page_t *pmm_paddr_to_page(paddr_t paddr);
uint64_t pmm_count_free_pages(void);
uint64_t pmm_count_total_pages(void);
void pmm_dump_state_counts(void);
const char *vm_page_state_name(vm_page_state_t state);

#endif /* ABOS_PMM_H */
