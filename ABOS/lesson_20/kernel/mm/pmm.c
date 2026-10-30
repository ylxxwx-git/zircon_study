/* =========================================================================
 * pmm.c - Zircon-Style PMM Using Higher-Half Physmap
 * ========================================================================= */
#include "pmm.h"
#include "mmu.h"
#include "spinlock.h"
#include "debuglog.h"

#define PMM_INVALID_IDX 0xFFFFFFFFU

static spin_lock_t pmm_lock = SPIN_LOCK_INITIAL_VALUE;
static paddr_t arena_base = 0;
static uint32_t arena_num_pages = 0;
static uint32_t free_list_head = PMM_INVALID_IDX;
static uint32_t free_page_count = 0;
static vm_page_t page_array[PMM_MAX_PAGES];

const char *vm_page_state_name(vm_page_state_t state) {
    switch (state) {
        case VM_PAGE_STATE_FREE:   return "FREE";
        case VM_PAGE_STATE_ALLOC:  return "ALLOC";
        case VM_PAGE_STATE_OBJECT: return "OBJECT(VMO)";
        case VM_PAGE_STATE_WIRED:  return "WIRED";
        case VM_PAGE_STATE_HEAP:   return "HEAP";
        case VM_PAGE_STATE_MMU:    return "MMU";
        case VM_PAGE_STATE_IPC:    return "IPC";
        default:                   return "UNKNOWN";
    }
}

static void zero_page_physmap(paddr_t pa) {
    uint64_t *p = (uint64_t *)paddr_to_physmap(pa);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        p[i] = 0;
    }
}

void pmm_init(paddr_t ram_base, uint64_t ram_size, paddr_t kernel_end_pa,
              paddr_t dtb_pa, uint32_t dtb_size,
              paddr_t initrd_start, paddr_t initrd_end) {
    arena_base = ram_base;
    uint64_t pages = ram_size / PAGE_SIZE;
    if (pages > PMM_MAX_PAGES) pages = PMM_MAX_PAGES;
    arena_num_pages = (uint32_t)pages;
    free_list_head  = PMM_INVALID_IDX;
    free_page_count = 0;

    paddr_t aligned_kern_end = (kernel_end_pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    paddr_t dtb_start = dtb_pa & ~(PAGE_SIZE - 1);
    paddr_t dtb_end   = (dtb_pa + dtb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    paddr_t rd_start  = initrd_start & ~(PAGE_SIZE - 1);
    paddr_t rd_end    = (initrd_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (int32_t i = (int32_t)arena_num_pages - 1; i >= 0; i--) {
        paddr_t pa = arena_base + (paddr_t)i * PAGE_SIZE;
        page_array[i].paddr     = pa;
        page_array[i].pin_count = 0;
        page_array[i].flags     = 0;

        int is_dtb    = (dtb_size > 0 && pa >= dtb_start && pa < dtb_end);
        int is_initrd = (initrd_end > initrd_start && pa >= rd_start && pa < rd_end);
        if (pa < aligned_kern_end || is_dtb || is_initrd) {
            page_array[i].state         = VM_PAGE_STATE_WIRED;
            page_array[i].next_free_idx = PMM_INVALID_IDX;
        } else {
            page_array[i].state         = VM_PAGE_STATE_FREE;
            page_array[i].next_free_idx = free_list_head;
            free_list_head              = (uint32_t)i;
            free_page_count++;
        }
    }

    dprintf(INFO, "[pmm] Arena initialized: base=%p pages=%u (%u free, %u wired)\n",
            (void *)arena_base, arena_num_pages,
            free_page_count, arena_num_pages - free_page_count);
}

vm_page_t *pmm_alloc_page(vm_page_state_t state, paddr_t *out_paddr) {
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&pmm_lock, &irqstate);

    if (free_list_head == PMM_INVALID_IDX) {
        spin_lock_irqrestore(&pmm_lock, irqstate);
        return NULL;
    }

    uint32_t idx = free_list_head;
    vm_page_t *page = &page_array[idx];
    free_list_head = page->next_free_idx;
    page->next_free_idx = PMM_INVALID_IDX;
    page->state = (uint8_t)state;
    page->pin_count = 0;
    free_page_count--;

    spin_lock_irqrestore(&pmm_lock, irqstate);

    zero_page_physmap(page->paddr);
    if (out_paddr) *out_paddr = page->paddr;
    return page;
}

vm_page_t *pmm_alloc_contiguous(size_t count, vm_page_state_t state, paddr_t *out_paddr) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page(state, out_paddr);

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&pmm_lock, &irqstate);

    uint32_t run_start = 0, run_len = 0;
    int found = 0;

    for (uint32_t i = 0; i < arena_num_pages; i++) {
        if (page_array[i].state == VM_PAGE_STATE_FREE) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len == count) {
                found = 1;
                break;
            }
        } else {
            run_len = 0;
        }
    }

    if (!found) {
        spin_lock_irqrestore(&pmm_lock, irqstate);
        return NULL;
    }

    for (uint32_t i = run_start; i < run_start + count; i++) {
        page_array[i].state = (uint8_t)state;
        page_array[i].pin_count = 0;
        page_array[i].next_free_idx = PMM_INVALID_IDX;
        free_page_count--;
    }

    free_list_head = PMM_INVALID_IDX;
    for (int32_t i = (int32_t)arena_num_pages - 1; i >= 0; i--) {
        if (page_array[i].state == VM_PAGE_STATE_FREE) {
            page_array[i].next_free_idx = free_list_head;
            free_list_head = (uint32_t)i;
        }
    }

    spin_lock_irqrestore(&pmm_lock, irqstate);

    for (size_t i = 0; i < count; i++) {
        zero_page_physmap(page_array[run_start + i].paddr);
    }

    if (out_paddr) *out_paddr = page_array[run_start].paddr;
    return &page_array[run_start];
}

void pmm_free_page(paddr_t paddr) {
    if (paddr < arena_base) return;
    uint32_t idx = (uint32_t)((paddr - arena_base) / PAGE_SIZE);
    if (idx >= arena_num_pages) return;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&pmm_lock, &irqstate);

    vm_page_t *page = &page_array[idx];
    if (page->state != VM_PAGE_STATE_FREE && page->state != VM_PAGE_STATE_WIRED) {
        page->state = VM_PAGE_STATE_FREE;
        page->pin_count = 0;
        page->next_free_idx = free_list_head;
        free_list_head = idx;
        free_page_count++;
    }

    spin_lock_irqrestore(&pmm_lock, irqstate);
}

void pmm_free_wired_range(paddr_t start_pa, paddr_t end_pa) {
    paddr_t s = start_pa & ~(PAGE_SIZE - 1);
    paddr_t e = (end_pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (e <= s) return;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&pmm_lock, &irqstate);
    for (paddr_t pa = s; pa < e; pa += PAGE_SIZE) {
        if (pa < arena_base) continue;
        uint32_t idx = (uint32_t)((pa - arena_base) / PAGE_SIZE);
        if (idx >= arena_num_pages) continue;
        vm_page_t *page = &page_array[idx];
        if (page->state == VM_PAGE_STATE_WIRED) {
            page->state         = VM_PAGE_STATE_FREE;
            page->pin_count     = 0;
            page->next_free_idx = free_list_head;
            free_list_head      = idx;
            free_page_count++;
        }
    }
    spin_lock_irqrestore(&pmm_lock, irqstate);
}

vm_page_t *pmm_paddr_to_page(paddr_t paddr) {
    if (paddr < arena_base) return NULL;
    uint32_t idx = (uint32_t)((paddr - arena_base) / PAGE_SIZE);
    if (idx >= arena_num_pages) return NULL;
    return &page_array[idx];
}

uint64_t pmm_count_free_pages(void) {
    return free_page_count;
}

uint64_t pmm_count_total_pages(void) {
    return arena_num_pages;
}

void pmm_dump_state_counts(void) {
    uint32_t counts[VM_PAGE_STATE_COUNT] = {0};
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&pmm_lock, &irqstate);
    for (uint32_t i = 0; i < arena_num_pages; i++) {
        if (page_array[i].state < VM_PAGE_STATE_COUNT) {
            counts[page_array[i].state]++;
        }
    }
    spin_lock_irqrestore(&pmm_lock, irqstate);

    dprintf(INFO, "[pmm] Page states: FREE=%u WIRED=%u HEAP=%u MMU=%u OBJECT=%u IPC=%u ALLOC=%u\n",
            counts[VM_PAGE_STATE_FREE], counts[VM_PAGE_STATE_WIRED],
            counts[VM_PAGE_STATE_HEAP], counts[VM_PAGE_STATE_MMU],
            counts[VM_PAGE_STATE_OBJECT], counts[VM_PAGE_STATE_IPC],
            counts[VM_PAGE_STATE_ALLOC]);
}
