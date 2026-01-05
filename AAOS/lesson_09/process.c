#include "process.h"
#include "pmm.h"
#include "mmu.h"
#include "output.h"

// Defined in boot.S
extern void move_to_user_mode(uint64_t entry_point, uint64_t stack_ptr);

extern char _binary_shell_bin_start[];
extern char _binary_shell_bin_end[];

void create_user_process(void) {
    printf("Creating User Process...\n");
    
    // Allocate page for user code
    void *user_page = pmm_alloc_page();
    if (!user_page) {
        printf("Failed to allocate user page\n");
        return;
    }
    
    // Calculate size
    uint64_t size = (uint64_t)_binary_shell_bin_end - (uint64_t)_binary_shell_bin_start;
    printf("Shell binary size: %d bytes\n", size);
    
    // Copy shell binary to user page
    // We need a memcpy implementation or loop
    char *src = _binary_shell_bin_start;
    char *dst = (char *)user_page;
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
    
    // Flush Cache
    asm volatile("dc cvau, %0" : : "r"(user_page));
    asm volatile("dsb ish");
    asm volatile("ic iallu");
    // Map the user page
    uint64_t va_offset = 0xFFFFFFFF40000000; // This was in the original code, but the instruction implies a different calculation. Sticking to original for PA calculation.
    uint64_t user_page_pa = (uint64_t)user_page - va_offset;
    // We want to map user_page_pa to 0x0 in user space
    // But map_user_page maps a physical address to a user virtual address?
    // Let's assume map_user_page maps PA to 0x0.
    // map_user_page implementation:
    // map_page(user_table, 0x0, pa, PTE_USER_RWX);
    map_user_page(user_page_pa);
    
    printf("Jumping to User Mode at 0x0...\n");
    // Stack pointer at 0x1000 (end of the 4KB page)
    move_to_user_mode(0x0, 0x1000); 
}
