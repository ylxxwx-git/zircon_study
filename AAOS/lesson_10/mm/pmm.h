#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096

// Initialize PMM with kernel end address and total RAM info
void pmm_init(uint64_t kernel_end, uint64_t ram_base, uint64_t ram_size);

// Allocate a single 4KB page
// Returns physical address or 0 if out of memory
void *pmm_alloc_page(void);

// Free a single 4KB page
void pmm_free_page(void *ptr);

#endif
