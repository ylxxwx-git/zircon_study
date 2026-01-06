#include "pmm.h"
#include "uart.h"
#include "print.h"
#include "string.h"

// Pointer to the bitmap (placed at kernel_end)
static uint8_t *bitmap = 0;

// Start of managed memory (aligned to page boundary after bitmap)
static uint64_t mem_start = 0;

// Total number of pages managed
static uint64_t page_count = 0;

// Helper: Align up
static uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}



#define VA_OFFSET 0xFFFFFFFF40000000

void pmm_init(uint64_t kernel_end, uint64_t ram_base, uint64_t ram_size) {
    // 1. Calculate total pages
    page_count = ram_size / PAGE_SIZE;
    uint64_t bitmap_size = page_count / 8;
    
    // 2. Place bitmap at kernel_end (which is a VA)
    bitmap = (uint8_t *)kernel_end;
    
    // 3. Initialize bitmap to 0 (all free)
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0;
    }
    
    // 4. Mark Kernel and Bitmap as USED
    // kernel_end is VA. Convert to PA for size calculation.
    uint64_t kernel_end_phys = kernel_end - VA_OFFSET;
    uint64_t used_end_phys = kernel_end_phys + bitmap_size;
    
    uint64_t used_pages = (used_end_phys - ram_base + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (uint64_t i = 0; i < used_pages; i++) {
        uint64_t byte_idx = i / 8;
        uint64_t bit_idx = i % 8;
        bitmap[byte_idx] |= (1 << bit_idx);
    }
    
    mem_start = ram_base; // Physical start of managed memory
    
    kputs("PMM Initialized.\n");
    kputs("Bitmap Address: "); kputhex((uint64_t)bitmap); kputs("\n");
    kputs("Managed Pages: "); kputhex(page_count); kputs("\n");
    kputs("Used Pages (Kernel+Bitmap): "); kputhex(used_pages); kputs("\n");
}

void *pmm_alloc_page(void) {
    // Find first free bit
    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t byte_idx = i / 8;
        uint64_t bit_idx = i % 8;
        
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            // Found free page
            bitmap[byte_idx] |= (1 << bit_idx);
            // Return Virtual Address
            return (void *)(mem_start + (i * PAGE_SIZE) + VA_OFFSET);
        }
    }
    return 0; // OOM
}

void pmm_free_page(void *ptr) {
    uint64_t addr = (uint64_t)ptr;
    
    // Convert VA to PA for checking
    if (addr >= VA_OFFSET) {
        addr -= VA_OFFSET;
    }
    
    if (addr < mem_start || addr >= mem_start + (page_count * PAGE_SIZE)) {
        return; // Invalid address
    }
    
    uint64_t page_idx = (addr - mem_start) / PAGE_SIZE;
    uint64_t byte_idx = page_idx / 8;
    uint64_t bit_idx = page_idx % 8;
    
    bitmap[byte_idx] &= ~(1 << bit_idx);
}
