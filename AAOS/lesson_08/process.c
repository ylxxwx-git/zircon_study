#include "process.h"
#include "pmm.h"
#include "mmu.h"
#include "output.h"

// Defined in boot.S
extern void move_to_user_mode(uint64_t entry_point, uint64_t stack_ptr);

void create_user_process(void) {
    printf("Creating User Process...\n");
    
    // Allocate page for user code
    void *user_page = pmm_alloc_page();
    if (!user_page) {
        printf("Failed to allocate user page\n");
        return;
    }
    
    // Write a small assembly program
    // 1. sys_write(0, "Hello", 5)
    // mov x0, #0       -> 0xD2800000
    // mov x1, #0x40    -> 0xD2800801 (Offset 64 bytes)
    // mov x2, #5       -> 0xD28000A2
    // mov x8, #64      -> 0xD2800808
    // svc #0           -> 0xD4000001
    
    // 2. sys_read(0, buf, 1)
    // mov x0, #0       -> 0xD2800000
    // mov x1, #0x40    -> 0xD2800801
    // mov x2, #1       -> 0xD2800022
    // mov x8, #63      -> 0xD28007E8
    // svc #0           -> 0xD4000001
    
    // 3. sys_exit(0)
    // mov x0, #0       -> 0xD2800000
    // mov x8, #93      -> 0xD2800BA8
    // svc #0           -> 0xD4000001
    
    uint32_t *code = (uint32_t *)user_page;
    int i = 0;
    
    // sys_write
    code[i++] = 0xD2800000;
    code[i++] = 0xD2800801;
    code[i++] = 0xD28000A2;
    code[i++] = 0xD2800808;
    code[i++] = 0xD4000001;
    
    // sys_read
    code[i++] = 0xD2800000;
    code[i++] = 0xD2800801;
    code[i++] = 0xD2800022;
    code[i++] = 0xD28007E8;
    code[i++] = 0xD4000001;
    
    // sys_exit
    code[i++] = 0xD2800000;
    code[i++] = 0xD2800BA8;
    code[i++] = 0xD4000001;
    
    // Data "Hello" at offset 64 (0x40)
    // 0x40 / 4 = 16
    char *data = (char *)&code[16];
    data[0] = 'H';
    data[1] = 'e';
    data[2] = 'l';
    data[3] = 'l';
    data[4] = 'o';
    data[5] = 0;
    
    // Flush Cache (Simple global invalidate for now)
    asm volatile("dc cvau, %0" : : "r"(user_page));
    asm volatile("dsb ish");
    asm volatile("ic iallu");
    asm volatile("dsb ish");
    asm volatile("isb");
    
    // Map it to 0x0
    // Convert VA to PA
    uint64_t va_offset = 0xFFFFFFFF40000000;
    uint64_t user_page_pa = (uint64_t)user_page - va_offset;
    map_user_page(user_page_pa);
    
    printf("Jumping to User Mode at 0x0...\n");
    move_to_user_mode(0x0, 0x1000); 
}
