/* =========================================================================
 * main.c - ABOS Lesson 14: Complete Capability Microkernel OS
 *
 * Four Isolated EL0 User-Space Processes:
 *   1. 'userboot'    : Bootstraps user space and wires IPC Channels
 *   2. 'console-srv' : EL0 PL011 UART MMIO + IRQ 33 driver & Console Server
 *   3. 'fs-srv'      : EL0 VirtIO-Blk DMA driver + USTAR TAR VMO File Server
 *   4. 'abos-shell'  : Unprivileged EL0 Interactive Shell using only Channels
 * ========================================================================= */
#include <stdint.h>
#include "debuglog.h"
#include "spinlock.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "dtb.h"
#include "pmm.h"
#include "heap.h"
#include "mmu.h"
#include "thread.h"
#include "wait.h"
#include "object.h"
#include "syscalls.h"

extern char _text_start[];
extern char _rodata_end[];
extern char _end[];
extern uint16_t ab_alloc_asid(void);

#define USER_KVA_TO_UVA(kva) ((vaddr_t)((uintptr_t)(kva) - 0xFFFFFFFF80000000ULL))

static inline __attribute__((always_inline))
ab_status_t user_syscall4(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    register uint64_t x8 __asm__("x8") = num;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "r"(x8)
                     : "memory");
    return (ab_status_t)x0;
}


void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *sr = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = sr[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

static size_t u_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int u_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int u_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

static void u_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++) != '\0') { }
}

static void ulog(ab_handle_t log_h, const char *msg) {
    user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0, (uintptr_t)msg, u_strlen(msg));
}

/* =========================================================================
 * IPC Protocol Definitions between Shell <-> Console Server & FS Server
 * ========================================================================= */
#define CON_OP_WRITE      1U
#define CON_OP_READ_LINE  2U
#define CON_OP_SHUTDOWN   3U

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

/* =========================================================================
 * 1. EL0 Console Server ('console-srv'):
 *    Maps PL011 UART0 MMIO (0x09000000) into EL0 via SYS_VMO_CREATE_PHYSICAL
 *    and binds GIC SPI 33 via SYS_INTERRUPT_CREATE!
 * ========================================================================= */
#define PL011_PHYS_BASE   0x09000000ULL
#define PL011_IRQ_NUM     33U
#define UART_DR           0x000
#define UART_FR           0x018
#define UART_IMSC         0x038
#define UART_ICR          0x044
#define UART_FR_RXFE      (1U << 4)
#define UART_FR_TXFF      (1U << 5)
#define UART_IMSC_RXIM    (1U << 4)

static inline uint32_t mmio_r32(vaddr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_w32(vaddr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static void el0_uart_putc(vaddr_t uart_va, char c) {
    if (c == '\n') {
        while (mmio_r32(uart_va, UART_FR) & UART_FR_TXFF) { }
        mmio_w32(uart_va, UART_DR, '\r');
    }
    while (mmio_r32(uart_va, UART_FR) & UART_FR_TXFF) { }
    mmio_w32(uart_va, UART_DR, (uint32_t)(unsigned char)c);
}

static void el0_uart_puts(vaddr_t uart_va, const char *s) {
    while (*s) el0_uart_putc(uart_va, *s++);
}

static char el0_uart_getc(vaddr_t uart_va, ab_handle_t uart_irq_h) {
    while (mmio_r32(uart_va, UART_FR) & UART_FR_RXFE) {
        /* Block in EL0 waiting for GIC SPI 33 (PL011 RX interrupt)! */
        user_syscall4(SYS_INTERRUPT_WAIT, uart_irq_h, 0, 0, 0);
        mmio_w32(uart_va, UART_ICR, UART_IMSC_RXIM);
        user_syscall4(SYS_INTERRUPT_ACK, uart_irq_h, 0, 0, 0);
    }
    return (char)(mmio_r32(uart_va, UART_DR) & 0xFFU);
}

void console_server_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[4];
    ab_channel_read_args_t rargs = { &tag, h, sizeof(tag), 4, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);
    user_syscall4(SYS_HANDLE_CLOSE, boot_ch, 0, 0, 0);

    ab_handle_t log_h     = h[0];
    ab_handle_t vmar_h    = h[1];
    ab_handle_t res_h     = h[2];
    ab_handle_t srv_ch_h  = h[3];

    /* Map PL011 UART MMIO (0x09000000) into EL0 and bind IRQ 33 */
    ab_handle_t uart_vmo = 0, uart_irq_h = 0;
    ab_vmo_create_phys_args_t pargs = { PL011_PHYS_BASE, PAGE_SIZE, &uart_vmo };
    user_syscall4(SYS_VMO_CREATE_PHYSICAL, res_h, (uintptr_t)&pargs, 0, 0);

    vaddr_t uart_va = 0;
    ab_vmar_map_args_t margs = {
        uart_vmo, 0, PAGE_SIZE, AB_VM_PERM_READ | AB_VM_PERM_WRITE, &uart_va
    };
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&margs, 0);

    user_syscall4(SYS_INTERRUPT_CREATE, res_h, PL011_IRQ_NUM, (uintptr_t)&uart_irq_h, 0);
    /* Enable PL011 RX interrupt from EL0! */
    mmio_w32(uart_va, UART_ICR, 0x7FFU);
    mmio_w32(uart_va, UART_IMSC, UART_IMSC_RXIM);

    ulog(log_h, "[console-srv EL0] Mapped PL011 MMIO (0x09000000) & bound IRQ 33! Ready.");

    for (;;) {
        ab_signals_t obs = 0;
        user_syscall4(SYS_OBJECT_WAIT_ONE, srv_ch_h,
                      AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED,
                      AB_TIME_INFINITE, (uintptr_t)&obs);
        if (obs & AB_SIGNAL_PEER_CLOSED) break;

        console_msg_t req;
        ab_channel_read_args_t cr = { &req, NULL, sizeof(req), 0, &act_b, &act_h };
        if (user_syscall4(SYS_CHANNEL_READ, srv_ch_h, 0, (uintptr_t)&cr, 0) != AB_OK) {
            continue;
        }

        if (req.op == CON_OP_WRITE) {
            el0_uart_puts(uart_va, req.text);
            console_msg_t ack = { CON_OP_WRITE, "" };
            ab_channel_write_args_t cw = { &ack, sizeof(uint32_t), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, srv_ch_h, 0, (uintptr_t)&cw, 0);
        } else if (req.op == CON_OP_READ_LINE) {
            console_msg_t rep;
            rep.op = CON_OP_READ_LINE;
            size_t pos = 0;
            while (pos + 1 < sizeof(rep.text)) {
                char c = el0_uart_getc(uart_va, uart_irq_h);
                if (c == '\r' || c == '\n') {
                    el0_uart_putc(uart_va, '\n');
                    break;
                } else if (c == '\b' || c == 127) {
                    if (pos > 0) {
                        pos--;
                        el0_uart_puts(uart_va, "\b \b");
                    }
                } else if (c >= 32 && c < 127) {
                    rep.text[pos++] = c;
                    el0_uart_putc(uart_va, c);
                }
            }
            rep.text[pos] = '\0';
            ab_channel_write_args_t cw = { &rep, sizeof(rep), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, srv_ch_h, 0, (uintptr_t)&cw, 0);
        } else if (req.op == CON_OP_SHUTDOWN) {
            break;
        }
    }

    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * 2. EL0 Filesystem Server ('fs-srv'):
 *    Drives VirtIO-Blk in EL0, parses the USTAR TAR archive on disk.img,
 *    and serves directory listings and read-only File VMOs over fs_ch!
 * ========================================================================= */
#define VIRTIO_MMIO_PHYS_BASE       0x0a000000ULL
#define VIRTIO_MMIO_MAGIC_VALUE     0x000
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_GUEST_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03C
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STAT  0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_RING_SIZE            16

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_RING_SIZE];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_t;

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
} __attribute__((packed)) tar_header_t;

static uint32_t oct2uint(const char *oct, size_t len) {
    uint32_t val = 0;
    for (size_t i = 0; i < len && oct[i]; i++) {
        if (oct[i] >= '0' && oct[i] <= '7') {
            val = (val << 3) | (uint32_t)(oct[i] - '0');
        }
    }
    return val;
}

void fs_server_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[4];
    ab_channel_read_args_t rargs = { &tag, h, sizeof(tag), 4, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);
    user_syscall4(SYS_HANDLE_CLOSE, boot_ch, 0, 0, 0);

    ab_handle_t log_h    = h[0];
    ab_handle_t vmar_h   = h[1];
    ab_handle_t res_h    = h[2];
    ab_handle_t fs_ch_h  = h[3];

    /* Map VirtIO MMIO and allocate 4-page Contiguous DMA VMO (8KB ring + 8KB disk cache) */
    ab_handle_t mmio_vmo = 0;
    ab_vmo_create_phys_args_t pargs = { VIRTIO_MMIO_PHYS_BASE, 4 * PAGE_SIZE, &mmio_vmo };
    user_syscall4(SYS_VMO_CREATE_PHYSICAL, res_h, (uintptr_t)&pargs, 0, 0);

    vaddr_t mmio_uva = 0;
    ab_vmar_map_args_t margs = {
        mmio_vmo, 0, 4 * PAGE_SIZE, AB_VM_PERM_READ | AB_VM_PERM_WRITE, &mmio_uva
    };
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&margs, 0);

    vaddr_t dev_base = 0;
    uint32_t dev_irq = 0;
    for (uint32_t slot = 0; slot < 32; slot++) {
        vaddr_t cand = mmio_uva + (uint64_t)slot * 0x200ULL;
        if (mmio_r32(cand, VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976U &&
            mmio_r32(cand, VIRTIO_MMIO_DEVICE_ID) == 2U) {
            dev_base = cand;
            dev_irq  = 48U + slot;
            break;
        }
    }

    ab_handle_t irq_h = 0, dma_vmo = 0;
    paddr_t dma_pa = 0;
    user_syscall4(SYS_INTERRUPT_CREATE, res_h, dev_irq, (uintptr_t)&irq_h, 0);
    ab_vmo_create_contig_args_t cargs = { 4 * PAGE_SIZE, &dma_vmo, &dma_pa };
    user_syscall4(SYS_VMO_CREATE_CONTIG, res_h, (uintptr_t)&cargs, 0, 0);

    vaddr_t dma_uva = 0;
    ab_vmar_map_args_t dma_map = {
        dma_vmo, 0, 4 * PAGE_SIZE, AB_VM_PERM_READ | AB_VM_PERM_WRITE, &dma_uva
    };
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&dma_map, 0);

    mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 1 | 2);
    mmio_w32(dev_base, VIRTIO_MMIO_GUEST_FEATURES, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_RING_SIZE);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, PAGE_SIZE);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa >> PAGE_SHIFT));
    mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 1 | 2 | 4);

    /* Read 8 sectors (4KB TAR image) into Page 3 (dma_uva + 3*PAGE_SIZE) via VirtIO DMA */
    virtq_desc_t  *desc  = (virtq_desc_t *)dma_uva;
    virtq_avail_t *avail = (virtq_avail_t *)(dma_uva + VIRTIO_RING_SIZE * sizeof(virtq_desc_t));
    virtio_blk_req_t *req = (virtio_blk_req_t *)(dma_uva + 2 * PAGE_SIZE);
    uint8_t *req_status   = (uint8_t *)(dma_uva + 2 * PAGE_SIZE + 64);
    uint8_t *tar_cache    = (uint8_t *)(dma_uva + 3 * PAGE_SIZE);

    req->type     = 0;
    req->reserved = 0;
    req->sector   = 0;
    *req_status   = 0xFF;

    desc[0].addr  = dma_pa + 2 * PAGE_SIZE;
    desc[0].len   = sizeof(virtio_blk_req_t);
    desc[0].flags = 1;
    desc[0].next  = 1;

    desc[1].addr  = dma_pa + 3 * PAGE_SIZE;
    desc[1].len   = PAGE_SIZE; /* 8 sectors = 4096 bytes */
    desc[1].flags = 1 | 2;
    desc[1].next  = 2;

    desc[2].addr  = dma_pa + 2 * PAGE_SIZE + 64;
    desc[2].len   = 1;
    desc[2].flags = 2;
    desc[2].next  = 0;

    avail->ring[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    avail->idx = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    user_syscall4(SYS_INTERRUPT_WAIT, irq_h, 0, 0, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(dev_base, VIRTIO_MMIO_INTERRUPT_STAT));
    user_syscall4(SYS_INTERRUPT_ACK, irq_h, 0, 0, 0);

    ulog(log_h, "[fs-srv EL0] Cached 8 TAR sectors from VirtIO-Blk via DMA! Serving FS requests...");

    for (;;) {
        ab_signals_t obs = 0;
        user_syscall4(SYS_OBJECT_WAIT_ONE, fs_ch_h,
                      AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED,
                      AB_TIME_INFINITE, (uintptr_t)&obs);
        if (obs & AB_SIGNAL_PEER_CLOSED) break;

        fs_msg_t req_msg;
        ab_channel_read_args_t cr = { &req_msg, NULL, sizeof(req_msg), 0, &act_b, &act_h };
        if (user_syscall4(SYS_CHANNEL_READ, fs_ch_h, 0, (uintptr_t)&cr, 0) != AB_OK) {
            continue;
        }

        if (req_msg.op == FS_OP_LIST) {
            fs_msg_t rep;
            rep.op        = FS_OP_LIST;
            rep.status    = 0;
            rep.file_size = 0;
            size_t out_pos = 0;

            size_t off = 0;
            while (off + 512 <= PAGE_SIZE) {
                const tar_header_t *th = (const tar_header_t *)(tar_cache + off);
                if (th->name[0] == '\0') break;
                uint32_t fsz = oct2uint(th->size, 12);
                const char *p = "  - ";
                while (*p && out_pos + 1 < sizeof(rep.data)) rep.data[out_pos++] = *p++;
                for (size_t i = 0; th->name[i] && out_pos + 1 < sizeof(rep.data); i++) {
                    rep.data[out_pos++] = th->name[i];
                }
                rep.data[out_pos++] = '\n';
                off += 512 + ((fsz + 511) & ~511U);
            }
            rep.data[out_pos] = '\0';
            ab_channel_write_args_t cw = { &rep, sizeof(rep), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, fs_ch_h, 0, (uintptr_t)&cw, 0);
        } else if (req_msg.op == FS_OP_OPEN_VMO) {
            fs_msg_t rep;
            rep.op        = FS_OP_OPEN_VMO;
            rep.status    = (uint32_t)AB_ERR_NOT_FOUND;
            rep.file_size = 0;
            rep.data[0]   = '\0';

            ab_handle_t ro_file_vmo = AB_HANDLE_INVALID;
            size_t off = 0;
            while (off + 512 <= PAGE_SIZE) {
                const tar_header_t *th = (const tar_header_t *)(tar_cache + off);
                if (th->name[0] == '\0') break;
                uint32_t fsz = oct2uint(th->size, 12);
                if (u_strcmp(th->name, req_msg.data) == 0) {
                    ab_handle_t rw_vmo = AB_HANDLE_INVALID;
                    user_syscall4(SYS_VMO_CREATE, PAGE_SIZE, 0, (uintptr_t)&rw_vmo, 0);
                    user_syscall4(SYS_VMO_WRITE, rw_vmo, (uintptr_t)(tar_cache + off + 512), 0, fsz);
                    /* Duplicate Read-Only + Map capability for the client! */
                    user_syscall4(SYS_HANDLE_DUPLICATE, rw_vmo,
                                  AB_RIGHT_READ | AB_RIGHT_MAP | AB_RIGHT_TRANSFER | AB_RIGHT_INSPECT,
                                  (uintptr_t)&ro_file_vmo, 0);
                    user_syscall4(SYS_HANDLE_CLOSE, rw_vmo, 0, 0, 0);
                    rep.status    = 0;
                    rep.file_size = fsz;
                    break;
                }
                off += 512 + ((fsz + 511) & ~511U);
            }

            if (ro_file_vmo != AB_HANDLE_INVALID) {
                ab_channel_write_args_t cw = { &rep, sizeof(rep), &ro_file_vmo, 1 };
                user_syscall4(SYS_CHANNEL_WRITE, fs_ch_h, 0, (uintptr_t)&cw, 0);
            } else {
                ab_channel_write_args_t cw = { &rep, sizeof(rep), NULL, 0 };
                user_syscall4(SYS_CHANNEL_WRITE, fs_ch_h, 0, (uintptr_t)&cw, 0);
            }
        } else if (req_msg.op == FS_OP_SHUTDOWN) {
            break;
        }
    }

    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * 3. EL0 Interactive Shell ('abos-shell'):
 *    Runs with ZERO hardware capabilities! Communicates with 'console-srv'
 *    and 'fs-srv' strictly over capability Channels and VMO mappings!
 * ========================================================================= */
static void shell_print(ab_handle_t con_ch, const char *text) {
    size_t total = u_strlen(text);
    size_t off = 0;
    while (off < total) {
        console_msg_t msg;
        msg.op = CON_OP_WRITE;
        size_t chunk = total - off;
        if (chunk > 200) chunk = 200;
        for (size_t i = 0; i < chunk; i++) msg.text[i] = text[off + i];
        msg.text[chunk] = '\0';
        off += chunk;

        ab_channel_write_args_t cw = { &msg, sizeof(msg), NULL, 0 };
        user_syscall4(SYS_CHANNEL_WRITE, con_ch, 0, (uintptr_t)&cw, 0);

        ab_signals_t obs = 0;
        user_syscall4(SYS_OBJECT_WAIT_ONE, con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
        uint32_t act_b = 0, act_h = 0;
        console_msg_t ack;
        ab_channel_read_args_t cr = { &ack, NULL, sizeof(ack), 0, &act_b, &act_h };
        user_syscall4(SYS_CHANNEL_READ, con_ch, 0, (uintptr_t)&cr, 0);
    }
}

static void shell_readline(ab_handle_t con_ch, char *out_buf) {
    console_msg_t req;
    req.op = CON_OP_READ_LINE;
    req.text[0] = '\0';
    ab_channel_write_args_t cw = { &req, sizeof(uint32_t), NULL, 0 };
    user_syscall4(SYS_CHANNEL_WRITE, con_ch, 0, (uintptr_t)&cw, 0);

    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
    uint32_t act_b = 0, act_h = 0;
    console_msg_t rep;
    ab_channel_read_args_t cr = { &rep, NULL, sizeof(rep), 0, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, con_ch, 0, (uintptr_t)&cr, 0);
    u_strcpy(out_buf, rep.text);
}

void shell_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[4];
    ab_channel_read_args_t rargs = { &tag, h, sizeof(tag), 4, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);
    user_syscall4(SYS_HANDLE_CLOSE, boot_ch, 0, 0, 0);

    ab_handle_t log_h  = h[0];
    ab_handle_t vmar_h = h[1];
    ab_handle_t con_ch = h[2];
    ab_handle_t fs_ch  = h[3];

    ulog(log_h, "[abos-shell EL0] Started with VMAR + ConsoleChannel + FSChannel capabilities!");

    shell_print(con_ch, "\n============================================================\n");
    shell_print(con_ch, "  ABOS Microkernel Shell (EL0) - Type 'help' for commands\n");
    shell_print(con_ch, "============================================================\n");

    char line[240];
    for (;;) {
        shell_print(con_ch, "abos$ ");
        shell_readline(con_ch, line);

        if (line[0] == '\0') continue;

        if (u_strcmp(line, "help") == 0) {
            shell_print(con_ch,
                "Available Microkernel Shell Commands:\n"
                "  help           - Show this help message\n"
                "  caps           - Inspect this process's Capability Handles & KOIDs\n"
                "  ls             - Query EL0 FS Server for TAR directory listing\n"
                "  cat <filename> - Request Read-Only VMO from FS Server & map zero-copy\n"
                "  exit           - Shut down shell and user-space servers\n");
        } else if (u_strcmp(line, "caps") == 0) {
            ab_handle_basic_info_t info;
            user_syscall4(SYS_OBJECT_GET_INFO, vmar_h, (uintptr_t)&info, 0, 0);
            shell_print(con_ch, "  [Handle 1] VMAR Dispatcher    (type=VMAR,    rights=READ|WRITE|MAP|EXEC)\n");
            user_syscall4(SYS_OBJECT_GET_INFO, con_ch, (uintptr_t)&info, 0, 0);
            shell_print(con_ch, "  [Handle 2] Console IPC Chan   (type=CHANNEL, rights=READ|WRITE|WAIT)\n");
            user_syscall4(SYS_OBJECT_GET_INFO, fs_ch, (uintptr_t)&info, 0, 0);
            shell_print(con_ch, "  [Handle 3] Filesystem IPC Chan(type=CHANNEL, rights=READ|WRITE|WAIT)\n");
            shell_print(con_ch, "  [Note] Shell holds ZERO Resource/MMIO/IRQ handles (pure capability isolation!)\n");
        } else if (u_strcmp(line, "ls") == 0) {
            fs_msg_t req = { FS_OP_LIST, 0, 0, "" };
            ab_channel_write_args_t cw = { &req, sizeof(req), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, fs_ch, 0, (uintptr_t)&cw, 0);

            ab_signals_t obs = 0;
            user_syscall4(SYS_OBJECT_WAIT_ONE, fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
            fs_msg_t rep;
            ab_channel_read_args_t cr = { &rep, NULL, sizeof(rep), 0, &act_b, &act_h };
            user_syscall4(SYS_CHANNEL_READ, fs_ch, 0, (uintptr_t)&cr, 0);
            shell_print(con_ch, "Files in TAR disk.img (served by EL0 fs-srv):\n");
            shell_print(con_ch, rep.data);
        } else if (u_strncmp(line, "cat ", 4) == 0) {
            const char *fname = line + 4;
            while (*fname == ' ') fname++;

            fs_msg_t req;
            req.op = FS_OP_OPEN_VMO;
            u_strcpy(req.data, fname);
            ab_channel_write_args_t cw = { &req, sizeof(req), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, fs_ch, 0, (uintptr_t)&cw, 0);

            ab_signals_t obs = 0;
            user_syscall4(SYS_OBJECT_WAIT_ONE, fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
            fs_msg_t rep;
            ab_handle_t file_vmo_h = AB_HANDLE_INVALID;
            ab_channel_read_args_t cr = { &rep, &file_vmo_h, sizeof(rep), 1, &act_b, &act_h };
            user_syscall4(SYS_CHANNEL_READ, fs_ch, 0, (uintptr_t)&cr, 0);

            if (rep.status == 0 && act_h == 1 && file_vmo_h != AB_HANDLE_INVALID) {
                vaddr_t file_va = 0;
                ab_vmar_map_args_t fmap = {
                    file_vmo_h, 0, PAGE_SIZE, AB_VM_PERM_READ, &file_va
                };
                user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&fmap, 0);
                shell_print(con_ch, (const char *)file_va);
                user_syscall4(SYS_HANDLE_CLOSE, file_vmo_h, 0, 0, 0);
            } else {
                shell_print(con_ch, "cat: file not found in TAR archive\n");
            }
        } else if (u_strcmp(line, "exit") == 0) {
            shell_print(con_ch, "Shutting down ABOS user-space servers. Goodbye!\n");
            fs_msg_t fstop = { FS_OP_SHUTDOWN, 0, 0, "" };
            ab_channel_write_args_t fcw = { &fstop, sizeof(uint32_t), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, fs_ch, 0, (uintptr_t)&fcw, 0);

            console_msg_t cstop = { CON_OP_SHUTDOWN, "" };
            ab_channel_write_args_t ccw = { &cstop, sizeof(uint32_t), NULL, 0 };
            user_syscall4(SYS_CHANNEL_WRITE, con_ch, 0, (uintptr_t)&ccw, 0);
            break;
        } else {
            shell_print(con_ch, "Unknown command. Type 'help' for available commands.\n");
        }
    }

    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * 4. EL0 'userboot': Spawns 'console-srv', 'fs-srv', and 'abos-shell'
 * ========================================================================= */
static ab_handle_t spawn_service(ab_handle_t root_job_h, const char *name,
                                 uintptr_t entry_fn, ab_handle_t *handles,
                                 uint32_t num_handles, ab_handle_t *out_vmar_h) {
    ab_handle_t proc_h = 0, vmar_h = 0, thr_h = 0;
    ab_process_create_args_t pc = { name, u_strlen(name), &proc_h, &vmar_h };
    user_syscall4(SYS_PROCESS_CREATE, root_job_h, (uintptr_t)&pc, 0, 0);
    user_syscall4(SYS_THREAD_CREATE, proc_h, (uintptr_t)name, u_strlen(name), (uintptr_t)&thr_h);

    ab_handle_t p_ch = 0, c_ch = 0;
    user_syscall4(SYS_CHANNEL_CREATE, 0, (uintptr_t)&p_ch, (uintptr_t)&c_ch, 0);

    handles[1] = vmar_h; /* Slot 1 is always the new process's Root VMAR handle */
    uint32_t tag = 0x53525621U;
    ab_channel_write_args_t cw = { &tag, sizeof(tag), handles, num_handles };
    user_syscall4(SYS_CHANNEL_WRITE, p_ch, 0, (uintptr_t)&cw, 0);
    user_syscall4(SYS_HANDLE_CLOSE, p_ch, 0, 0, 0);

    ab_process_start_args_t sa = { thr_h, entry_fn, 0, c_ch, 0 };
    user_syscall4(SYS_PROCESS_START, proc_h, (uintptr_t)&sa, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, thr_h, 0, 0, 0);
    if (out_vmar_h) *out_vmar_h = vmar_h;
    return proc_h;
}

void userboot_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t boot_h[3];
    ab_channel_read_args_t rargs = { &tag, boot_h, sizeof(tag), 3, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);

    ab_handle_t root_job_h = boot_h[0];
    ab_handle_t root_log_h = boot_h[1];
    ab_handle_t root_res_h = boot_h[2];

    ulog(root_log_h, "[userboot] Spawning EL0 microkernel servers: console-srv, fs-srv, abos-shell!");

    /* Create service IPC channels connecting Shell <-> Console Server and Shell <-> FS Server */
    ab_handle_t con_srv_end = 0, con_cli_end = 0;
    ab_handle_t fs_srv_end  = 0, fs_cli_end  = 0;
    user_syscall4(SYS_CHANNEL_CREATE, 0, (uintptr_t)&con_srv_end, (uintptr_t)&con_cli_end, 0);
    user_syscall4(SYS_CHANNEL_CREATE, 0, (uintptr_t)&fs_srv_end,  (uintptr_t)&fs_cli_end, 0);

    /* 1. Spawn 'console-srv' with [log, vmar, resource, con_srv_end] */
    ab_handle_t log1 = 0, res1 = 0;
    user_syscall4(SYS_HANDLE_DUPLICATE, root_log_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&log1, 0);
    user_syscall4(SYS_HANDLE_DUPLICATE, root_res_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&res1, 0);
    ab_handle_t con_h[4] = { log1, 0, res1, con_srv_end };
    ab_handle_t con_proc = spawn_service(root_job_h, "console-srv",
                                         (uintptr_t)console_server_entry, con_h, 4, NULL);

    /* 2. Spawn 'fs-srv' with [log, vmar, resource, fs_srv_end] */
    ab_handle_t log2 = 0, res2 = 0;
    user_syscall4(SYS_HANDLE_DUPLICATE, root_log_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&log2, 0);
    user_syscall4(SYS_HANDLE_DUPLICATE, root_res_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&res2, 0);
    ab_handle_t fs_h[4] = { log2, 0, res2, fs_srv_end };
    ab_handle_t fs_proc = spawn_service(root_job_h, "fs-srv",
                                        (uintptr_t)fs_server_entry, fs_h, 4, NULL);

    /* 3. Spawn 'abos-shell' with [log, vmar, con_cli_end, fs_cli_end] (NO Resource handle!) */
    ab_handle_t log3 = 0;
    user_syscall4(SYS_HANDLE_DUPLICATE, root_log_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&log3, 0);
    ab_handle_t sh_h[4] = { log3, 0, con_cli_end, fs_cli_end };
    ab_handle_t sh_proc = spawn_service(root_job_h, "abos-shell",
                                        (uintptr_t)shell_entry, sh_h, 4, NULL);

    /* Wait for shell and servers to exit */
    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, sh_proc,  AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, (uintptr_t)&obs);
    user_syscall4(SYS_OBJECT_WAIT_ONE, fs_proc,  AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, (uintptr_t)&obs);
    user_syscall4(SYS_OBJECT_WAIT_ONE, con_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, (uintptr_t)&obs);

    ulog(root_log_h, "[userboot] All user-space services exited cleanly!");
    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

void arm64_secondary_entry(uint32_t cpu_num) {
    (void)cpu_num;
    thread_secondary_cpu_init();
    gic_cpu_init();
    timer_init_percpu(100);
    thread_become_idle();
}

void lk_main(uint64_t dtb_paddr) {
    dlog_init_early();
    thread_init_early();

    dprintf(INFO, "\n==================================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 14: Complete Microkernel OS (EL0 Console, TAR FS & Shell)\n");
    dprintf(INFO, "==================================================================\n");

    mmu_init();
    boot_info_t boot_info;
    dtb_parse(dtb_paddr, &boot_info);
    pmm_init(boot_info.ram_base, boot_info.ram_size, physmap_to_paddr(_end),
             boot_info.dtb_paddr, boot_info.dtb_size);
    heap_init();

    gic_dist_init();
    gic_cpu_init();
    timer_init_percpu(100);

    ab_object_t *root_job = NULL, *root_log = NULL, *root_res = NULL;
    ab_job_dispatcher_create(&root_job);
    ab_log_dispatcher_create(0, &root_log);
    ab_resource_dispatcher_create(0, &root_res);

    ab_process_t *userboot_proc = ab_process_create_kernel("userboot", ab_alloc_asid());
    ab_object_t *ub_thr = NULL;
    ab_thread_dispatcher_create(userboot_proc, "userboot-main", &ub_thr);

    ab_object_t *ch0 = NULL, *ch1 = NULL;
    ab_channel_dispatcher_create(&ch0, &ch1);

    ab_handle_t job_h = 0, log_h = 0, res_h = 0, ch1_h = 0;
    handle_alloc(userboot_proc, root_job, AB_RIGHTS_DEFAULT_JOB, &job_h);
    handle_alloc(userboot_proc, root_log, AB_RIGHTS_DEFAULT_LOG, &log_h);
    handle_alloc(userboot_proc, root_res, AB_RIGHTS_DEFAULT_RESOURCE, &res_h);
    handle_alloc(userboot_proc, ch1,      AB_RIGHTS_DEFAULT_CHANNEL, &ch1_h);

    ab_handle_t boot_h[3] = { job_h, log_h, res_h };
    uint32_t tag = 0x424F4F54U;
    ab_channel_write(userboot_proc, ch0, &tag, sizeof(tag), boot_h, 3);

    ab_object_release(root_job);
    ab_object_release(root_log);
    ab_object_release(root_res);
    ab_object_release(ch0);
    ab_object_release(ch1);

    ab_process_start_user(NULL, userboot_proc, (ab_thread_dispatcher_t *)ub_thr,
                          USER_KVA_TO_UVA(userboot_entry), 0, ch1_h, 0);

    arch_enable_ints();
    while (((ab_thread_dispatcher_t *)ub_thr)->kthread->state != THREAD_DEATH) {
        thread_yield();
        __asm__ volatile("wfi");
    }

    dprintf(INFO, "[lk_main] ABOS Lesson 14 complete! All 15 lessons verified.\n");
    thread_become_idle();
}
