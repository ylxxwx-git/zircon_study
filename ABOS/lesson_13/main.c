/* =========================================================================
 * main.c - ABOS Lesson 13: User-Space Device Drivers (Resource, Physical MMIO
 *          VMO, Contiguous DMA VMO & InterruptDispatcher in EL0!)
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

static size_t user_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void ulog(ab_handle_t log_h, const char *msg) {
    user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0, (uintptr_t)msg, user_strlen(msg));
}

/* =========================================================================
 * VirtIO MMIO Register & Ring Definitions (used entirely in EL0 user space!)
 * ========================================================================= */
#define VIRTIO_MMIO_PHYS_BASE       0x0a000000ULL
#define VIRTIO_MMIO_MAGIC_VALUE     0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_HOST_FEATURES   0x010
#define VIRTIO_MMIO_GUEST_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03C
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STAT  0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070

#define VIRTIO_STATUS_ACK           1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4

#define VRING_DESC_F_NEXT           1
#define VRING_DESC_F_WRITE          2
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
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t          flags;
    uint16_t          idx;
    virtq_used_elem_t ring[VIRTIO_RING_SIZE];
} __attribute__((packed)) virtq_used_t;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_t;

static inline uint32_t mmio_read32(vaddr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(vaddr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

/* =========================================================================
 * EL0 User-Space VirtIO Block Driver Process ('virtio-blk-drv')
 * Runs in unprivileged EL0! Uses Resource, Physical MMIO VMO, Contiguous
 * DMA VMO, and InterruptDispatcher capabilities!
 * ========================================================================= */
void virtio_driver_entry(uint64_t arg0) {
    ab_handle_t drv_ch = (ab_handle_t)arg0;

    /* 1. Receive capabilities from userboot over drv_ch: [log_h, vmar_h, root_res_h] */
    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[3];
    ab_channel_read_args_t rargs = { &tag, h, sizeof(tag), 3, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, drv_ch, 0, (uintptr_t)&rargs, 0);

    ab_handle_t log_h  = h[0];
    ab_handle_t vmar_h = h[1];
    ab_handle_t res_h  = h[2];

    ulog(log_h, "[virtio-drv EL0] Started! Mapping VirtIO MMIO (0x0a000000) via SYS_VMO_CREATE_PHYSICAL...");

    /* 2. Map VirtIO MMIO (0x0a000000, 16KB) into EL0 via Physical VMO */
    ab_handle_t mmio_vmo = AB_HANDLE_INVALID;
    ab_vmo_create_phys_args_t pargs = { VIRTIO_MMIO_PHYS_BASE, 4 * PAGE_SIZE, &mmio_vmo };
    user_syscall4(SYS_VMO_CREATE_PHYSICAL, res_h, (uintptr_t)&pargs, 0, 0);

    vaddr_t mmio_uva = 0;
    ab_vmar_map_args_t margs = {
        mmio_vmo, 0, 4 * PAGE_SIZE, AB_VM_PERM_READ | AB_VM_PERM_WRITE, &mmio_uva
    };
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&margs, 0);

    /* 3. Probe VirtIO MMIO transports in EL0 to locate Block Device (DeviceID == 2) */
    vaddr_t dev_base = 0;
    uint32_t dev_irq = 0;
    for (uint32_t slot = 0; slot < 32; slot++) {
        vaddr_t cand = mmio_uva + (uint64_t)slot * 0x200ULL;
        if (mmio_read32(cand, VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976U &&
            mmio_read32(cand, VIRTIO_MMIO_DEVICE_ID) == 2U) {
            dev_base = cand;
            dev_irq  = 48U + slot; /* QEMU virt GIC SPI 16 + slot = 48 + slot */
            break;
        }
    }

    ulog(log_h, "[virtio-drv EL0] Discovered VirtIO Block Device in EL0! Binding IRQ via SYS_INTERRUPT_CREATE...");

    /* 4. Bind hardware IRQ to an InterruptDispatcher handle */
    ab_handle_t irq_h = AB_HANDLE_INVALID;
    user_syscall4(SYS_INTERRUPT_CREATE, res_h, dev_irq, (uintptr_t)&irq_h, 0);

    /* 5. Allocate 3-page Contiguous DMA VMO (Pages 0..1 = Virtqueue, Page 2 = Req + Sector Data) */
    ab_handle_t dma_vmo = AB_HANDLE_INVALID;
    paddr_t dma_pa = 0;
    ab_vmo_create_contig_args_t cargs = { 3 * PAGE_SIZE, &dma_vmo, &dma_pa };
    user_syscall4(SYS_VMO_CREATE_CONTIG, res_h, (uintptr_t)&cargs, 0, 0);

    vaddr_t dma_uva = 0;
    ab_vmar_map_args_t dma_map = {
        dma_vmo, 0, 3 * PAGE_SIZE, AB_VM_PERM_READ | AB_VM_PERM_WRITE, &dma_uva
    };
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&dma_map, 0);

    /* 6. Initialize VirtIO Block Device & Queue 0 from EL0 */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, 0);
    uint32_t st_bits = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, st_bits);
    mmio_write32(dev_base, VIRTIO_MMIO_GUEST_FEATURES, 0);
    mmio_write32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);

    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_RING_SIZE);
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, PAGE_SIZE);
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa >> PAGE_SHIFT));
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, st_bits | VIRTIO_STATUS_DRIVER_OK);

    /* 7. Submit Sector 0 Read Request via DMA Virtqueue */
    virtq_desc_t  *desc  = (virtq_desc_t *)dma_uva;
    virtq_avail_t *avail = (virtq_avail_t *)(dma_uva + VIRTIO_RING_SIZE * sizeof(virtq_desc_t));
    /* Page 2 (offset 8192) holds request header, 512-byte sector buffer, and status byte */
    vaddr_t  req_uva  = dma_uva + 2 * PAGE_SIZE;
    paddr_t  req_pa   = dma_pa  + 2 * PAGE_SIZE;
    virtio_blk_req_t *req = (virtio_blk_req_t *)req_uva;
    uint8_t *sector_buf   = (uint8_t *)(req_uva + 64);
    uint8_t *req_status   = (uint8_t *)(req_uva + 64 + 512);

    req->type     = 0; /* VIRTIO_BLK_T_IN (Read) */
    req->reserved = 0;
    req->sector   = 0;
    *req_status   = 0xFF;

    desc[0].addr  = req_pa;
    desc[0].len   = sizeof(virtio_blk_req_t);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = req_pa + 64;
    desc[1].len   = 512;
    desc[1].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;
    desc[1].next  = 2;

    desc[2].addr  = req_pa + 64 + 512;
    desc[2].len   = 1;
    desc[2].flags = VRING_DESC_F_WRITE;
    desc[2].next  = 0;

    avail->ring[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    avail->idx = 1;
    __asm__ volatile("dsb sy" ::: "memory");

    ulog(log_h, "[virtio-drv EL0] Kicking QueueNotify and blocking in SYS_INTERRUPT_WAIT...");
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* 8. Block in EL0 waiting for the hardware interrupt object! */
    user_syscall4(SYS_INTERRUPT_WAIT, irq_h, 0, 0, 0);

    /* Acknowledge device interrupt in MMIO & unmask GIC via SYS_INTERRUPT_ACK */
    uint32_t isr = mmio_read32(dev_base, VIRTIO_MMIO_INTERRUPT_STAT);
    mmio_write32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    user_syscall4(SYS_INTERRUPT_ACK, irq_h, 0, 0, 0);

    ulog(log_h, "[virtio-drv EL0] Woke from hardware IRQ! Sector 0 DMA buffer contents:");
    ulog(log_h, (const char *)sector_buf);

    /* Send Sector 0 contents over Channel back to userboot */
    ab_channel_write_args_t wargs = {
        sector_buf, (uint32_t)(user_strlen((const char *)sector_buf) + 1), NULL, 0
    };
    user_syscall4(SYS_CHANNEL_WRITE, drv_ch, 0, (uintptr_t)&wargs, 0);

    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * EL0 'userboot': Spawns 'virtio-blk-drv' and delegates Root Resource!
 * ========================================================================= */
void userboot_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t boot_h[3];
    ab_channel_read_args_t rargs = { &tag, boot_h, sizeof(tag), 3, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);

    ab_handle_t root_job_h = boot_h[0];
    ab_handle_t root_log_h = boot_h[1];
    ab_handle_t root_res_h = boot_h[2];

    ulog(root_log_h, "[userboot] Received RootJob, RootLog, and RootResource from kernel!");

    /* Create 'virtio-blk-drv' process in EL0 */
    ab_handle_t drv_proc_h = 0, drv_vmar_h = 0, drv_thr_h = 0;
    const char *pname = "virtio-blk-drv";
    ab_process_create_args_t pc_args = { pname, user_strlen(pname), &drv_proc_h, &drv_vmar_h };
    user_syscall4(SYS_PROCESS_CREATE, root_job_h, (uintptr_t)&pc_args, 0, 0);

    const char *tname = "virtio-drv-main";
    user_syscall4(SYS_THREAD_CREATE, drv_proc_h, (uintptr_t)tname, user_strlen(tname), (uintptr_t)&drv_thr_h);

    ab_handle_t parent_ch = 0, child_ch = 0;
    user_syscall4(SYS_CHANNEL_CREATE, 0, (uintptr_t)&parent_ch, (uintptr_t)&child_ch, 0);

    ab_handle_t dup_log_h = 0;
    user_syscall4(SYS_HANDLE_DUPLICATE, root_log_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&dup_log_h, 0);

    ab_handle_t send_h[3] = { dup_log_h, drv_vmar_h, root_res_h };
    uint32_t drv_tag = 0x44525621U;
    ab_channel_write_args_t wargs = { &drv_tag, sizeof(drv_tag), send_h, 3 };
    user_syscall4(SYS_CHANNEL_WRITE, parent_ch, 0, (uintptr_t)&wargs, 0);

    ab_process_start_args_t sargs = {
        drv_thr_h, (uintptr_t)virtio_driver_entry, 0, child_ch, 0
    };
    user_syscall4(SYS_PROCESS_START, drv_proc_h, (uintptr_t)&sargs, 0, 0);

    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, parent_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
    char sector_reply[128];
    ab_channel_read_args_t rep_rargs = { sector_reply, NULL, sizeof(sector_reply), 0, &act_b, &act_h };
    if (user_syscall4(SYS_CHANNEL_READ, parent_ch, 0, (uintptr_t)&rep_rargs, 0) == AB_OK) {
        ulog(root_log_h, "[userboot] Verified Sector 0 reply from EL0 VirtIO Block Driver!");
    }

    user_syscall4(SYS_OBJECT_WAIT_ONE, drv_proc_h, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, (uintptr_t)&obs);
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
    dprintf(INFO, "Lesson 13: User-Space Drivers (Resource, Physical/DMA VMO & IRQ)\n");
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
    while (((ab_thread_dispatcher_t *)ub_thr)->kthread->state != THREAD_DEATH) { thread_yield(); __asm__ volatile("wfi"); } //

    dprintf(INFO, "[lk_main] EL0 User-space VirtIO driver test complete! Lesson 13 complete.\n");
    thread_become_idle();
}
