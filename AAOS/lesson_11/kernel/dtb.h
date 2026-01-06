#ifndef DTB_H
#define DTB_H

#include <stdint.h>

#define FDT_MAGIC 0xd00dfeed

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

// Initialize and parse DTB
void dtb_init(uint64_t dtb_ptr);

// Helper to convert Big Endian to CPU Endian (Little Endian)
uint32_t be32_to_cpu(uint32_t be);

#endif
