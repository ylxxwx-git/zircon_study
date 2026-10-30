/* =========================================================================
 * syscalls.h - Zircon-Style Capability, Signal & Port System Calls
 * ========================================================================= */
#ifndef ABOS_SYSCALLS_H
#define ABOS_SYSCALLS_H

#include <stdint.h>
#include "object.h"
#include "exceptions.h"

#define SYS_HANDLE_CLOSE        1
#define SYS_HANDLE_DUPLICATE    2
#define SYS_OBJECT_GET_INFO     3
#define SYS_DEBUGLOG_WRITE      4
#define SYS_PROCESS_EXIT        5
#define SYS_NANOSLEEP           6
#define SYS_EVENT_CREATE        7
#define SYS_EVENTPAIR_CREATE    8
#define SYS_OBJECT_SIGNAL       9
#define SYS_OBJECT_SIGNAL_PEER  10
#define SYS_OBJECT_WAIT_ONE     11
#define SYS_PORT_CREATE         12
#define SYS_PORT_QUEUE          13
#define SYS_PORT_WAIT           14
#define SYS_OBJECT_WAIT_ASYNC   15
#define SYS_CHANNEL_CREATE      16
#define SYS_CHANNEL_WRITE       17
#define SYS_CHANNEL_READ        18
#define SYS_VMO_CREATE          19
#define SYS_VMO_READ            20
#define SYS_VMO_WRITE           21
#define SYS_VMAR_MAP            22
#define SYS_PROCESS_CREATE      23
#define SYS_THREAD_CREATE       24
#define SYS_PROCESS_START       25
#define SYS_VMO_CREATE_PHYSICAL 26
#define SYS_VMO_CREATE_CONTIG   27
#define SYS_INTERRUPT_CREATE    28
#define SYS_INTERRUPT_WAIT      29
#define SYS_INTERRUPT_ACK       30
#define SYS_OBJECT_SET_NAME     31
#define SYS_JOB_GET_PS_INFO     32
#define SYS_PROCESS_GET_MAPS    33
#define SYS_VMAR_UNMAP          34

void syscall_dispatch(iframe_t *frame);
void ab_process_exit_current(int exit_code);

/* Safe EL0 <-> EL1 user memory copy helpers */
ab_status_t copy_from_user(void *kdst, const void *usrc, size_t len);
ab_status_t copy_to_user(void *udst, const void *ksrc, size_t len);

#endif /* ABOS_SYSCALLS_H */
