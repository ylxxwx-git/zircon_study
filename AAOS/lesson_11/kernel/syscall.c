#include "syscall.h"
#include "uart.h"
#include "print.h"
#include "string.h"
#include "process.h"
#include "sched.h"
#include "irq.h"

void sys_write(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We assume fd=1 (stdout)
    char *buf = (char *)regs->x1;
    int len = regs->x2;
    
    for (int i = 0; i < len; i++) {
        uart_write_char(buf[i]);
    }
    // printk("sys_write: wrote %d bytes from %p\n", len, buf);
}


void sys_read(struct pt_regs *regs) {
    // x0 = fd, x1 = buf, x2 = len
    // We only support fd=0 (stdin) for now
    char *buf = (char *)regs->x1;
    // int len = regs->x2;
    
    // Block until char received
    char c = uart_read_char();
    
    // Write to user buffer
    *buf = c;
    
    // Debug print
    uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    // printk("sys_read: daif=%lx, buf=%p\n", daif, buf);
    
    // Block until char received
    
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

void svc_handler_c(struct pt_regs *regs, uint64_t esr) {
    // uint64_t esr;
    // asm volatile("mrs %0, esr_el1" : "=r"(esr));
    
    // Check if it's an SVC instruction (EC = 0x15)
    if ((esr >> 26) == 0x15) {
        // System Call
        uint64_t syscall_num = regs->x8;
        
        // Handle specific syscalls
        // printk("syscall: %d, x0=%p, x1=%p, x2=%p, lr=%p\n", syscall_num, (void*)regs->x0, (void*)regs->x1, (void*)regs->x2, (void*)regs->x30);
        
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
        
        // Advance ELR to skip the SVC instruction - NOT NEEDED for SVC (ELR is already PC+4)
        // regs->elr += 4;

    } else {
        uint64_t elr, far;
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        asm volatile("mrs %0, far_el1" : "=r"(far));
        printk("Unexpected Sync Exception: ESR=%x ELR=%x FAR=%x\n", esr, elr, far);
        while(1);
    }
}
