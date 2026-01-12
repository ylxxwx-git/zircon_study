#include "syscall.h"
#include "uart.h"
#include "print.h"
#include "string.h"
#include "process.h"
#include "sched.h"
#include "irq.h"

#include "tty.h"

void sys_write(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We assume fd=1 (stdout)
    char *buf = (char *)regs->x1;
    int len = regs->x2;
    int core_id = get_core_id();
    
    for (int i = 0; i < len; i++) {
        tty_write(core_id, buf[i]);
    }
}

void sys_read(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We only support fd=0 (stdin) for now
    char *buf = (char *)regs->x1;
    // int len = regs->x2;
    int core_id = get_core_id();
    
    // Block until char received
    char c = tty_read_char(core_id);
    
    // Write to user buffer
    *buf = c;
    
    // Return 1 (byte read)
    regs->x0 = 1;
}

void sys_exit(struct pt_regs *regs) {
    printk("sys_exit called with code=%d\n", regs->x0);
    kthread_exit();
}

void sys_yield(struct pt_regs *regs) {
    schedule();
}

#include "mmu.h"
#include "pmm.h"

void handle_data_abort(struct pt_regs *regs, uint64_t esr) {
    uint64_t far;
    asm volatile("mrs %0, far_el1" : "=r"(far));
    
    printk("Data Abort: FAR=%x ESR=%x PC=%x\n", far, esr, regs->elr);
    
    // Check if it's a Translation Fault (Level 0, 1, 2, or 3)
    // ISS[5:0] is DFSC (Data Fault Status Code)
    // 0b000100 (0x04) = Translation fault, level 0
    // 0b000101 (0x05) = Translation fault, level 1
    // 0b000110 (0x06) = Translation fault, level 2
    // 0b000111 (0x07) = Translation fault, level 3
    
    uint64_t dfsc = esr & 0x3F;
    if (dfsc >= 0x04 && dfsc <= 0x07) {
        // It's a Translation Fault (Page not mapped)
        
        // Check if address is within valid User Stack range
        // For now, we assume anything below 0x80000000 is valid user space
        // and if it's unmapped, it's a stack growth or heap.
        // Let's be generous for this lesson.
        
        if (far < 0x80000000) {
            printk("Lazy Allocation for %x\n", far);
            
            // Allocate Physical Page
            void *page = pmm_alloc_page();
            if (!page) {
                printk("OOM! Failed to allocate page for lazy allocation.\n");
                while(1);
            }
            
            // Map it
            // Align FAR to 4KB
            uint64_t far_aligned = far & ~0xFFF;
            uint64_t va_offset = 0xFFFFFFFF40000000;
            uint64_t pa = (uint64_t)page - va_offset;
            
            map_user_page(far_aligned, pa);
            
            return; // Retry instruction
        }
    }
    
    printk("Unhandled Data Abort.\n");
    while(1);
}

void sync_handler_c(struct pt_regs *regs, uint64_t esr) {
    uint64_t ec = esr >> 26;
    
    if (ec == 0x15) {
        // System Call (SVC)
        uint64_t syscall_num = regs->x8;
        
        switch (syscall_num) {
            case 63: // sys_read
                sys_read(regs);
                break;
            case 64: // sys_write
                sys_write(regs);
                break;
            case 93: // sys_exit
                sys_exit(regs);
                break;
            case 124: // sys_yield
                sys_yield(regs);
                break;
            default:
                printk("Unknown Syscall: %d\n", syscall_num);
                break;
        }
    } else if (ec == 0x24) {
        // Data Abort from Lower EL
        handle_data_abort(regs, esr);
    } else {
        uint64_t elr, far;
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        asm volatile("mrs %0, far_el1" : "=r"(far));
        printk("Unexpected Sync Exception: EC=%x ESR=%x ELR=%x FAR=%x\n", ec, esr, elr, far);
        while(1);
    }
}
