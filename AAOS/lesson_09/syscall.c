#include "syscall.h"
#include "output.h"

#include "sched.h"

void sys_write(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We assume fd=1 (stdout)
    char *buf = (char *)regs->x1;
    int len = regs->x2;
    
    for (int i = 0; i < len; i++) {
        putc(buf[i]);
    }
    // printf("sys_write: wrote %d bytes from %p\n", len, buf);
}


void sys_read(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We only support fd=0 (stdin) for now
    char *buf = (char *)regs->x1;
    // int len = regs->x2;
    
    // Block until char received
    char c = uart_read();
    
    // Write to user buffer
    *buf = c;
    
    // Debug print
    uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    // printf("sys_read: daif=%lx, buf=%p\n", daif, buf);
    
    // Block until char received
    
    // Return 1 (byte read)
    regs->x0 = 1;
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
        // printf("syscall: %d, x0=%p, x1=%p, x2=%p, lr=%p\n", syscall_num, (void*)regs->x0, (void*)regs->x1, (void*)regs->x2, (void*)regs->x30);
        
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
                printf("Unknown Syscall: %d\n", syscall_num);
                break;
        }
        
        // Advance ELR to skip the SVC instruction - NOT NEEDED for SVC (ELR is already PC+4)
        // regs->elr += 4;
    } else {
        printf("Unexpected Sync Exception: ESR=%x\n", esr);
        while(1);
    }
}
