#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>

// Stack frame structure matches stp order in boot.S
#include "irq.h"

void svc_handler_c(struct pt_regs *regs, uint64_t esr);

#endif
