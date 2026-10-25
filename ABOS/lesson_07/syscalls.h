/* =========================================================================
 * syscalls.h - ABOS (Zircon-Style) System Call Numbers & Prototypes
 * References:
 *   - zircon_org/system/public/zircon/syscalls.abigen
 * ========================================================================= */
#ifndef ABOS_SYSCALLS_H
#define ABOS_SYSCALLS_H

#include "object.h"
#include "exceptions.h"

#define SYS_HANDLE_CLOSE        1
#define SYS_HANDLE_DUPLICATE    2
#define SYS_HANDLE_REPLACE      3
#define SYS_OBJECT_GET_INFO     4
#define SYS_DEBUGLOG_CREATE     5
#define SYS_DEBUGLOG_WRITE      6
#define SYS_PROCESS_EXIT        7

void syscall_dispatch(iframe_t *iframe);

#endif /* ABOS_SYSCALLS_H */
