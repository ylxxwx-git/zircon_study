#include "process.h"
#include "pmm.h"
#include "mmu.h"
#include "uart.h"
#include "print.h"
#include "string.h"
#include "tar.h"

// Defined in boot.S
extern void move_to_user_mode(uint64_t entry_point, uint64_t stack_ptr);

extern char _binary_shell_bin_start[];
extern char _binary_shell_bin_end[];

void create_user_process(const char *filename) {
    printk("Creating User Process: %s\n", filename);
    
    // Allocate page for user code
    void *user_page = pmm_alloc_page();
    if (!user_page) {
        printk("Failed to allocate user page\n");
        return;
    }
    
    // Find file in TAR
    uint64_t sector;
    uint32_t size;
    if (tar_find(filename, &sector, &size) != 0) {
        printk("File not found: %s\n", filename);
        return;
    }
    
    printk("Loading %s from sector %d (size: %d)\n", filename, sector, size);
    
    // Read file content
    if (tar_read(sector, size, user_page) != 0) {
        printk("Failed to read file\n");
        return;
    }
    
    // Flush Cache
    asm volatile("dc cvau, %0" : : "r"(user_page));
    asm volatile("dsb ish");
    asm volatile("ic iallu");
    asm volatile("isb");
    
    // Map the user page at VA 0x0
    uint64_t va_offset = 0xFFFFFFFF40000000;
    uint64_t user_page_pa = (uint64_t)user_page - va_offset;
    map_user_page(0x0, user_page_pa);
    
    printk("Jumping to User Mode at 0x0...\n");
    // Stack pointer at Top of User Space (0x80000000)
    // We do NOT map the stack here. It will be mapped lazily on first access.
    move_to_user_mode(0x0, 0x80000000); 
}
