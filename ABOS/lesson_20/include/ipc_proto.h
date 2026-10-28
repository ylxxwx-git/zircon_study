#ifndef ABOS_IPC_PROTO_H
#define ABOS_IPC_PROTO_H

#include <stdint.h>
#include "object.h"
#include "syscalls.h"

#define USER_KVA_TO_UVA(kva) ((vaddr_t)((uintptr_t)(kva) - 0xFFFFFFFF80000000ULL))

/* =========================================================================
 * IPC Protocol Definitions between Shell <-> Console Server & FS Server
 * ========================================================================= */
#define NUM_VTS           4U

#define CON_OP_WRITE      1U
#define CON_OP_READ_LINE  2U
#define CON_OP_SHUTDOWN   3U
#define CON_OP_SWITCH_VT  4U

typedef struct {
    uint32_t op;
    char     text[240];
} console_msg_t;

#define FS_OP_LIST        1U
#define FS_OP_OPEN_VMO    2U
#define FS_OP_SHUTDOWN    3U

typedef struct {
    uint32_t op;
    uint32_t status;
    uint32_t file_size;
    char     data[236];
} fs_msg_t;

/* Embedded userboot bootstrap entry point */
void userboot_entry(uint64_t arg0);

#endif /* ABOS_IPC_PROTO_H */
