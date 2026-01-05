#include "virtio.h"
#include "output.h"
#include "pmm.h"
#include "utils.h"

#define VIRTIO_RING_SIZE 16
#define VA_OFFSET 0xFFFFFFFF40000000

static volatile uint8_t *virtio_base = (uint8_t *)VIRTIO_MMIO_BASE;

// Queue structures
static struct virtq_desc *desc;
static struct virtq_avail *avail;
static struct virtq_used *used;

static uint16_t free_head;
static uint16_t used_idx;

static uint32_t virtio_reg_read32(uint32_t offset) {
    return *(volatile uint32_t *)(virtio_base + offset);
}

static void virtio_reg_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(virtio_base + offset) = value;
}

void virtio_init(void) {
    int found_device = 0;
    for (uint64_t base = VIRTIO_MMIO_BASE; base < VIRTIO_MMIO_BASE + 0x4000; base += 0x200) {
        virtio_base = (uint8_t *)base;
        uint32_t magic = virtio_reg_read32(VIRTIO_MMIO_MAGIC_VALUE);
        uint32_t device_id = virtio_reg_read32(VIRTIO_MMIO_DEVICE_ID);
        
        if (magic == 0x74726976) {
            printf("VirtIO: Found Device ID %d at %p\n", device_id, (void*)base);
            if (device_id == 2) {
                printf("VirtIO: Using Block Device at %p\n", (void*)base);
                found_device = 1;
                goto found;
            }
        }
    }
    
    if (!found_device) {
        printf("VirtIO: No Block Device found!\n");
        return;
    }

found:
    uint32_t version = virtio_reg_read32(VIRTIO_MMIO_VERSION);
    printf("VirtIO: Version=%x\n", version);

    // Read Device Features
    uint32_t features = virtio_reg_read32(VIRTIO_MMIO_DEVICE_FEATURES);
    printf("VirtIO: Device Features=%x\n", features);

    // 1. Reset device
    virtio_reg_write32(VIRTIO_MMIO_STATUS, 0);

    // 2. Set ACKNOWLEDGE status bit
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    virtio_reg_write32(VIRTIO_MMIO_STATUS, status);

    // 3. Set DRIVER status bit
    status |= VIRTIO_STATUS_DRIVER;
    virtio_reg_write32(VIRTIO_MMIO_STATUS, status);

    // 4. Negotiate features (accept none)
    virtio_reg_write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    
    // 5. Set FEATURES_OK status bit (Only for Version 2+)
    if (version >= 2) {
        status |= VIRTIO_STATUS_FEATURES_OK;
        virtio_reg_write32(VIRTIO_MMIO_STATUS, status);
        
        // Check if device accepts features
        if (!(virtio_reg_read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            printf("VirtIO: Feature negotiation failed!\n");
            return;
        }
    }

    // 6. Setup Queues
    // Select Queue 0
    virtio_reg_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    
    uint32_t max_num = virtio_reg_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max_num == 0) {
        printf("VirtIO: Queue not available!\n");
        return;
    }
    
    // Set Queue Num
    virtio_reg_write32(VIRTIO_MMIO_QUEUE_NUM, VIRTIO_RING_SIZE);
    printf("VirtIO: QueueNumMax=%d, Set QueueNum=%d, ReadBack=%d\n", max_num, VIRTIO_RING_SIZE, virtio_reg_read32(VIRTIO_MMIO_QUEUE_NUM));
    
    // Set Queue Align
    virtio_reg_write32(0x03C, 4096); // VIRTIO_MMIO_QUEUE_ALIGN

    // Allocate Queue Memory
    // Legacy requirement: Used ring must be aligned to 4096 bytes
    // We need 2 pages: Page 0 for Desc+Avail, Page 1 for Used
    // Use PMM to allocate pages. For Legacy, they must be contiguous.
    void *page1 = pmm_alloc_page();
    void *page2 = pmm_alloc_page();
    
    if (!page1 || !page2) {
        printf("VirtIO: Failed to allocate ring memory!\n");
        return;
    }
    
    // Check contiguity for Legacy
    if (version == 1 && (uint64_t)page2 != (uint64_t)page1 + 4096) {
        printf("VirtIO: Ring memory not contiguous! p1=%p p2=%p\n", page1, page2);
        // In a real driver, we would free and retry or handle non-contiguous for Modern
        return;
    }
    
    void *ring_mem = page1;
    // heap_ptr += 8192; // Removed bump allocator
    
    // Zero out memory
    memset(ring_mem, 0, 8192);
    
    desc = (struct virtq_desc *)ring_mem;
    avail = (struct virtq_avail *)((uint8_t *)ring_mem + VIRTIO_RING_SIZE * 16);
    
    // Used ring at offset 4096
    used = (struct virtq_used *)((uint8_t *)ring_mem + 4096);

    // Write Physical Addresses to Device
    // Legacy interface uses PFN (Page Frame Number) for Queue Address?
    // Wait, QEMU virt 0.04 uses Legacy or Modern?
    // Version 1 is Legacy. Version 2 is Modern.
    // If Version == 2, we use QUEUE_DESC_LOW/HIGH etc.
    // If Version == 1, we use QUEUE_PFN (Offset 0x040)
    
    if (version == 1) {
        // Legacy Setup
        virtio_reg_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        
        // PFN = Address / 4096
        // ring_mem is a Virtual Address (High Half). Convert to Physical.
        uint64_t ring_pa = (uint64_t)ring_mem - VA_OFFSET;
        printf("VirtIO: Ring PA=%p, PFN=%x\n", (void*)ring_pa, ring_pa / 4096);
        virtio_reg_write32(0x040, ring_pa / 4096); // VIRTIO_MMIO_QUEUE_PFN
    } else {
        // Modern Setup (Version 2+)
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint64_t)desc);
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint64_t)desc >> 32);
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint64_t)avail);
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint64_t)avail >> 32);
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_USED_LOW, (uint64_t)used);
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint64_t)used >> 32);
        
        virtio_reg_write32(VIRTIO_MMIO_QUEUE_READY, 1);
    }

    // 7. Set DRIVER_OK status bit
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_reg_write32(VIRTIO_MMIO_STATUS, status);
    
    // Initialize free list
    for (int i = 0; i < VIRTIO_RING_SIZE - 1; i++) {
        desc[i].next = i + 1;
    }
    desc[VIRTIO_RING_SIZE - 1].next = 0; // End of list
    free_head = 0;
    used_idx = 0;
    
    printf("VirtIO: Block Device Initialized!\n");
}

int virtio_blk_read(uint64_t sector, void *buffer, uint32_t count) {
    // Simplified: Read one sector at a time or multiple?
    // Let's support count sectors.
    
    // We need 3 descriptors:
    // 1. Request Header (Write to device)
    // 2. Data Buffer (Read from device)
    // 3. Status (Read from device)
    
    // Allocate 3 descriptors
    // In a real driver, we need locking here.
    
    uint16_t head = free_head;
    uint16_t d1 = head;
    uint16_t d2 = desc[d1].next;
    uint16_t d3 = desc[d2].next;
    
    // Update free_head
    free_head = desc[d3].next;
    
    // Update free_head
    free_head = desc[d3].next;
    
    // Allocate req and status dynamically
    static struct virtio_blk_req *req = NULL;
    static uint8_t *status = NULL;
    
    if (!req) {
        req = (struct virtio_blk_req *)pmm_alloc_page();
        if (!req) return -1;
        status = (uint8_t *)req + sizeof(struct virtio_blk_req);
    }
    
    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    
    *status = 0; // Reset status
    
    // Descriptor 1: Request Header
    uint64_t req_pa = (uint64_t)req - VA_OFFSET;
    desc[d1].addr = req_pa;
    desc[d1].len = sizeof(struct virtio_blk_req);
    desc[d1].flags = VRING_DESC_F_NEXT;
    desc[d1].next = d2;
    
    // Descriptor 2: Data Buffer
    uint64_t buffer_pa = (uint64_t)buffer - VA_OFFSET;
    desc[d2].addr = buffer_pa;
    desc[d2].len = count * VIRTIO_BLK_SECTOR_SIZE;
    desc[d2].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;
    desc[d2].next = d3;
    
    // Descriptor 3: Status
    uint64_t status_pa = (uint64_t)status - VA_OFFSET;
    desc[d3].addr = status_pa;
    desc[d3].len = 1;
    desc[d3].flags = VRING_DESC_F_WRITE;
    desc[d3].next = 0;
    
    // Add to Avail Ring
    avail->ring[avail->idx % VIRTIO_RING_SIZE] = d1;
    asm volatile("dsb sy"); // Stronger Memory Barrier
    avail->idx++;
    asm volatile("dsb sy");
    
    // Notify Device
    virtio_reg_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    
    // Busy Wait for completion
    // In a real driver, we would use interrupts.
    int timeout = 1000000;
    static int debug_printed = 0;
    while (used->idx == used_idx && timeout > 0) {
        if (timeout % 100000 == 0) {
            uint32_t dev_status = virtio_reg_read32(VIRTIO_MMIO_STATUS);
            printf("VirtIO: Waiting... used->idx=%d used_idx=%d Status=%x\n", used->idx, used_idx, dev_status);
            
            if (!debug_printed) {
                printf("VirtIO Debug:\n");
                printf("  Desc: %p, Avail: %p, Used: %p\n", desc, avail, used);
                printf("  Req PA: %p, Buf PA: %p, Status PA: %p\n", (void*)req_pa, (void*)buffer_pa, (void*)status_pa);
                printf("  Queue Num: %d\n", virtio_reg_read32(VIRTIO_MMIO_QUEUE_NUM));
                printf("  Queue Ready: %d\n", virtio_reg_read32(VIRTIO_MMIO_QUEUE_READY));
                printf("  Interrupt Status: %x\n", virtio_reg_read32(VIRTIO_MMIO_INTERRUPT_STATUS));
                debug_printed = 1;
            }
            
            if (dev_status & VIRTIO_STATUS_DEVICE_NEEDS_RESET) {
                printf("VirtIO: Device needs reset!\n");
                return -1;
            }
            if (dev_status & VIRTIO_STATUS_FAILED) {
                printf("VirtIO: Device failed!\n");
                return -1;
            }
        }
        timeout--;
        asm volatile("nop");
    }
    
    if (timeout == 0) {
        printf("VirtIO: Read timed out! used->idx=%d used_idx=%d\n", used->idx, used_idx);
        return -1;
    }
    
    used_idx++;
    
    if (*status != 0) {
        // printf("VirtIO: Read failed! Status Addr=%p Value=%x\n", status, *status);
        return -1;
    }
    
    // Return descriptors to free list
    desc[d3].next = free_head;
    free_head = d1;
    
    return 0;
}
