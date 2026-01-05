#include "syscall.h"
#include "output.h"

#include "sched.h"

void sys_write(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    int len = regs->x2;
    printf("sys_write called with len=%d\n", len);
}

void sys_read(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We only support fd=0 (stdin) for now
    // We only support reading 1 char for now (or loop)
    
    // char *buf = (char *)regs->x1; // Unsafe
    int len = regs->x2;
    
    // For now, just read one char and return it in x0 (not standard, but easy for test)
    // Standard read returns number of bytes read.
    // And puts bytes in buf.
    // Since we can't write to buf safely yet (need copy_to_user),
    // we will just return the char in x0 for testing.
    
    char c = uart_read();
    regs->x0 = c; // Return char in x0
    printf("sys_read got '%c'\n", c);
}

void sys_exit(struct pt_regs *regs) {
    printf("sys_exit called with code=%d\n", regs->x0);
    kthread_exit();
}

void sys_yield(struct pt_regs *regs) {
    schedule();
}

void svc_handler_c(struct pt_regs *regs) {
    uint64_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    
    // Check if it's an SVC instruction (EC = 0x15)
    if ((esr >> 26) == 0x15) {
        // System Call
        uint64_t syscall_num = regs->x8;
        
        // Handle specific syscalls
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
            case 24: // sys_yield
                sys_yield(regs);
                break;
            default:
                printf("Unknown Syscall: %d\n", syscall_num);
                break;
        }
        
        // Advance ELR to skip the SVC instruction
        regs->elr += 4;
    } else {
        printf("Unexpected Sync Exception: ESR=%x\n", esr);
        while(1);
    }
}
