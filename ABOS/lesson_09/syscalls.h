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

void syscall_dispatch(iframe_t *frame);

#endif /* ABOS_SYSCALLS_H */
