/* =========================================================================
 * dtb.h - Flattened Device Tree (FDT) / Boot Handoff Parser
 * ========================================================================= */
#ifndef ABOS_DTB_H
#define ABOS_DTB_H

#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedU

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

typedef struct {
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t dtb_paddr;
    uint32_t dtb_size;
    uint64_t initrd_start;
    uint64_t initrd_end;
} boot_info_t;

int dtb_parse(uint64_t dtb_paddr, boot_info_t *out_info);

#endif /* ABOS_DTB_H */
