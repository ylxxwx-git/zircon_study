#include "tar.h"
#include "virtio.h"
#include "output.h"
#include "utils.h"
#include "pmm.h"

// We assume the disk is just a TAR file.
// Sector 0 is the first header.

static uint8_t sector_buffer[512] __attribute__((aligned(16)));

// Helper to parse octal size
static uint32_t parse_octal(const char *str, int len) {
    uint32_t size = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] < '0' || str[i] > '7') break;
        size = size * 8 + (str[i] - '0');
    }
    return size;
}

void tar_init(void) {
    printf("TAR: Reading sector 0...\n");
    
    // Use dynamic buffer to ensure valid RAM
    void *buffer = pmm_alloc_page();
    if (!buffer) {
        printf("TAR: OOM\n");
        return;
    }
    
    // Just verify we can read sector 0
    if (virtio_blk_read(0, buffer, 1) != 0) {
        printf("TAR: Failed to read sector 0\n");
        return;
    }
    struct tar_header *header = (struct tar_header *)buffer;
    
    printf("TAR: First file: %s\n", header->name);
    printf("TAR: Magic: %s\n", header->magic);
}

int tar_find(const char *name, uint64_t *start_sector, uint32_t *file_size) {
    uint64_t sector = 0;
    
    while (1) {
        // printf("TAR: Checking sector %d\n", (int)sector);
        if (virtio_blk_read(sector, sector_buffer, 1) != 0) {
            // printf("TAR: Read error at sector %d\n", (int)sector);
            return -1;
        }
        struct tar_header *header = (struct tar_header *)sector_buffer;
        
        if (header->name[0] == 0) {
            // printf("TAR: End of archive at sector %d\n", (int)sector);
            return -1; // End of archive
        }
        
        // printf("TAR: Found file: %s\n", header->name);
        
        uint32_t size = parse_octal(header->size, 11);
        
        if (strcmp(header->name, name) == 0) {
            *start_sector = sector + 1; // Content starts after header
            *file_size = size;
            return 0;
        }
        
        // Skip content sectors
        uint32_t sectors = (size + 511) / 512;
        sector += 1 + sectors;
    }
}

int tar_read(uint64_t sector, uint32_t size, void *buffer) {
    uint32_t sectors = (size + 511) / 512;
    return virtio_blk_read(sector, buffer, sectors);
}
