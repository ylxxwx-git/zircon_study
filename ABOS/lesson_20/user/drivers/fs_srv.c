/* =========================================================================
 * drivers/fs_srv.c - EL0 Multi-Client VirtIO-Blk & TAR Filesystem Server
 *
 * Lesson 20: Standalone BOOTFS system server linked at VA 0x20000000.
 * Multiplexes 4 virtual terminal shell channels (tty0..tty3) concurrently
 * using a Zircon PortDispatcher (sys_port_create + sys_object_wait_async).
 * ========================================================================= */
#include "ulibc.h"

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
#define VIRTIO_MMIO_CONFIG          0x100
#define VIRTIO_RING_SIZE            16

#define FS_DMA_TOTAL_PAGES          32U
#define FS_DMA_META_PAGES           3U
#define FS_TAR_CACHE_PAGES          (FS_DMA_TOTAL_PAGES - FS_DMA_META_PAGES)
#define FS_TAR_CACHE_MAX_BYTES      (FS_TAR_CACHE_PAGES * PAGE_SIZE)

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

static inline uint32_t mmio_r32(vaddr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_w32(vaddr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static uint32_t oct2uint(const char *oct, size_t len) {
    uint32_t val = 0;
    for (size_t i = 0; i < len && oct[i]; i++) {
        if (oct[i] >= '0' && oct[i] <= '7') {
            val = (val << 3) | (uint32_t)(oct[i] - '0');
        }
    }
    return val;
}

static int handle_fs_request(ab_handle_t ch_h, const uint8_t *tar_cache,
                             size_t cache_bytes, const fs_msg_t *req_msg) {
    if (req_msg->op == FS_OP_LIST) {
        fs_msg_t rep;
        rep.op        = FS_OP_LIST;
        rep.status    = 0;
        rep.file_size = 0;
        size_t out_pos = 0;

        size_t off = 0;
        while (off + 512 <= cache_bytes) {
            const tar_header_t *th = (const tar_header_t *)(tar_cache + off);
            if (th->name[0] == '\0') break;
            uint32_t fsz = oct2uint(th->size, 12);
            const char *p = "  - ";
            while (*p && out_pos + 1 < sizeof(rep.data)) rep.data[out_pos++] = *p++;
            for (size_t i = 0; th->name[i] && out_pos + 1 < sizeof(rep.data); i++) {
                rep.data[out_pos++] = th->name[i];
            }
            if (out_pos + 1 < sizeof(rep.data)) rep.data[out_pos++] = '\n';
            off += 512 + ((fsz + 511U) & ~511U);
        }
        rep.data[out_pos] = '\0';
        sys_channel_write(ch_h, &rep, sizeof(rep), NULL, 0);
        return 0;
    } else if (req_msg->op == FS_OP_OPEN_VMO) {
        fs_msg_t rep;
        rep.op        = FS_OP_OPEN_VMO;
        rep.status    = (uint32_t)AB_ERR_NOT_FOUND;
        rep.file_size = 0;
        rep.data[0]   = '\0';

        ab_handle_t ro_file_vmo = AB_HANDLE_INVALID;
        size_t off = 0;
        while (off + 512 <= cache_bytes) {
            const tar_header_t *th = (const tar_header_t *)(tar_cache + off);
            if (th->name[0] == '\0') break;
            uint32_t fsz = oct2uint(th->size, 12);
            if (u_strcmp(th->name, req_msg->data) == 0 &&
                off + 512 + fsz <= cache_bytes) {
                ab_handle_t rw_vmo = AB_HANDLE_INVALID;
                uint64_t vmo_sz = ((uint64_t)fsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (vmo_sz == 0) vmo_sz = PAGE_SIZE;
                if (sys_vmo_create(vmo_sz, 0, &rw_vmo) == AB_OK) {
                    sys_object_set_name(rw_vmo, th->name);
                    sys_vmo_write(rw_vmo, tar_cache + off + 512, 0, fsz);
                    sys_handle_duplicate(rw_vmo,
                                         AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_MAP |
                                         AB_RIGHT_EXECUTE | AB_RIGHT_TRANSFER | AB_RIGHT_INSPECT,
                                         &ro_file_vmo);
                    sys_handle_close(rw_vmo);
                    rep.status    = 0;
                    rep.file_size = fsz;
                }
                break;
            }
            off += 512 + ((fsz + 511U) & ~511U);
        }

        if (ro_file_vmo != AB_HANDLE_INVALID) {
            sys_channel_write(ch_h, &rep, sizeof(rep), &ro_file_vmo, 1);
        } else {
            sys_channel_write(ch_h, &rep, sizeof(rep), NULL, 0);
        }
        return 0;
    } else if (req_msg->op == FS_OP_SHUTDOWN) {
        return 1;
    }
    return 0;
}

int main(void) {
    uint32_t act_b = 0, act_h = 0;

    ab_handle_t log_h  = ulibc_get_startup_handle(0);
    ab_handle_t vmar_h = ulibc_get_startup_handle(1);
    ab_handle_t res_h  = ulibc_get_startup_handle(2);
    ab_handle_t fs_ch[NUM_VTS];
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        fs_ch[i] = ulibc_get_startup_handle(3 + i);
    }

    /* Map VirtIO MMIO and allocate 32-page (128KB) Contiguous DMA VMO */
    ab_handle_t mmio_vmo = 0;
    sys_vmo_create_physical(res_h, VIRTIO_MMIO_PHYS_BASE, 4 * PAGE_SIZE, &mmio_vmo);
    sys_object_set_name(mmio_vmo, "virtio-blk-mmio");

    vaddr_t mmio_uva = 0;
    sys_vmar_map(vmar_h, 0, mmio_vmo, 0, 4 * PAGE_SIZE,
                 AB_VM_PERM_READ | AB_VM_PERM_WRITE, &mmio_uva);

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

    if (dev_base == 0) {
        ulog(log_h, "[fs-srv EL0] ERROR: No VirtIO-Blk device found!");
        return -1;
    }

    uint64_t cap_lo = mmio_r32(dev_base, VIRTIO_MMIO_CONFIG);
    uint64_t cap_hi = mmio_r32(dev_base, VIRTIO_MMIO_CONFIG + 4);
    uint64_t disk_bytes = ((cap_hi << 32) | cap_lo) * 512ULL;
    size_t cache_bytes = FS_TAR_CACHE_MAX_BYTES;
    if (disk_bytes > 0 && disk_bytes < cache_bytes) {
        cache_bytes = (size_t)disk_bytes;
    }

    ab_handle_t irq_h = 0, dma_vmo = 0;
    paddr_t dma_pa = 0;
    sys_interrupt_create(res_h, dev_irq, &irq_h);
    if (sys_vmo_create_contiguous(res_h, FS_DMA_TOTAL_PAGES * PAGE_SIZE,
                                  &dma_vmo, &dma_pa) != AB_OK) {
        ulog(log_h, "[fs-srv EL0] ERROR: Failed to allocate Contiguous DMA VMO!");
        return -1;
    }
    sys_object_set_name(dma_vmo, "virtio-blk-dma");

    vaddr_t dma_uva = 0;
    sys_vmar_map(vmar_h, 0, dma_vmo, 0, FS_DMA_TOTAL_PAGES * PAGE_SIZE,
                 AB_VM_PERM_READ | AB_VM_PERM_WRITE, &dma_uva);

    mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 1 | 2);
    mmio_w32(dev_base, VIRTIO_MMIO_GUEST_FEATURES, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_RING_SIZE);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, PAGE_SIZE);
    mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa >> PAGE_SHIFT));
    mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 1 | 2 | 4);

    virtq_desc_t  *desc  = (virtq_desc_t *)dma_uva;
    virtq_avail_t *avail = (virtq_avail_t *)(dma_uva + VIRTIO_RING_SIZE * sizeof(virtq_desc_t));
    virtio_blk_req_t *req = (virtio_blk_req_t *)(dma_uva + 2 * PAGE_SIZE);
    volatile uint8_t *req_status = (volatile uint8_t *)(dma_uva + 2 * PAGE_SIZE + 64);
    uint8_t *tar_cache    = (uint8_t *)(dma_uva + FS_DMA_META_PAGES * PAGE_SIZE);

    req->type     = 0;
    req->reserved = 0;
    req->sector   = 0;
    *req_status   = 0xFF;

    desc[0].addr  = dma_pa + 2 * PAGE_SIZE;
    desc[0].len   = sizeof(virtio_blk_req_t);
    desc[0].flags = 1;
    desc[0].next  = 1;

    desc[1].addr  = dma_pa + FS_DMA_META_PAGES * PAGE_SIZE;
    desc[1].len   = (uint32_t)cache_bytes;
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

    sys_interrupt_wait(irq_h);
    mmio_w32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(dev_base, VIRTIO_MMIO_INTERRUPT_STAT));
    sys_interrupt_ack(irq_h);

    if (*req_status != 0) {
        ulog(log_h, "[fs-srv EL0] ERROR: VirtIO-Blk DMA read returned non-zero status!");
        cache_bytes = 0;
    } else {
        dlog_printf("[fs-srv EL0] Cached %lu TAR sectors (%lu KB) via DMA! Multiplexing 4 VT channels on Port...",
                    (unsigned long)(cache_bytes / 512), (unsigned long)(cache_bytes / 1024));
    }

    ab_handle_t port_h = AB_HANDLE_INVALID;
    sys_port_create(0, &port_h);
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        sys_object_wait_async(fs_ch[i], port_h, (uint64_t)i,
                              AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
    }

    int running = 1;
    while (running) {
        ab_port_packet_t pkt;
        if (sys_port_wait(port_h, AB_TIME_INFINITE, &pkt) != AB_OK) {
            continue;
        }
        uint32_t idx = (uint32_t)pkt.key;
        if (idx >= NUM_VTS || fs_ch[idx] == AB_HANDLE_INVALID) {
            continue;
        }

        fs_msg_t req_msg;
        while (sys_channel_read(fs_ch[idx], &req_msg, sizeof(req_msg),
                                NULL, 0, &act_b, &act_h) == AB_OK) {
            if (handle_fs_request(fs_ch[idx], tar_cache, cache_bytes, &req_msg)) {
                running = 0;
                break;
            }
        }

        if (!running) break;

        if (pkt.observed & AB_SIGNAL_PEER_CLOSED) {
            sys_handle_close(fs_ch[idx]);
            fs_ch[idx] = AB_HANDLE_INVALID;
        } else {
            sys_object_wait_async(fs_ch[idx], port_h, (uint64_t)idx,
                                  AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
        }
    }

    return 0;
}
