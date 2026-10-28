/* =========================================================================
 * dtb.c - Flattened Device Tree Parser via Higher-Half Physmap
 * ========================================================================= */
#include "dtb.h"
#include "mmu.h"
#include "debuglog.h"

#define FDT_BEGIN_NODE 0x00000001U
#define FDT_END_NODE   0x00000002U
#define FDT_PROP       0x00000003U
#define FDT_NOP        0x00000004U
#define FDT_END        0x00000009U

static uint32_t be32_to_cpu(uint32_t be) {
    return ((be & 0xFF000000U) >> 24) |
           ((be & 0x00FF0000U) >> 8)  |
           ((be & 0x0000FF00U) << 8)  |
           ((be & 0x000000FFU) << 24);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static uintptr_t align4(uintptr_t p) {
    return (p + 3U) & ~((uintptr_t)3U);
}

int dtb_parse(uint64_t dtb_paddr, boot_info_t *out_info) {
    out_info->ram_base     = 0x40000000ULL;
    out_info->ram_size     = 0x08000000ULL;
    out_info->dtb_paddr    = dtb_paddr;
    out_info->dtb_size     = 0;
    out_info->initrd_start = 0;
    out_info->initrd_end   = 0;

    if (dtb_paddr < 0x40000000ULL || dtb_paddr >= 0x80000000ULL) {
        dprintf(INFO, "[dtb] Using default 128MB RAM arena\n");
        return -1;
    }

    uintptr_t dtb_va = (uintptr_t)paddr_to_physmap(dtb_paddr);
    const struct fdt_header *hdr = (const struct fdt_header *)dtb_va;
    if (be32_to_cpu(hdr->magic) != FDT_MAGIC) {
        dprintf(INFO, "[dtb] Invalid FDT magic at PA %p\n", (void *)dtb_paddr);
        return -1;
    }

    out_info->dtb_size = be32_to_cpu(hdr->totalsize);
    uintptr_t struct_ptr   = dtb_va + be32_to_cpu(hdr->off_dt_struct);
    uintptr_t struct_end   = struct_ptr + be32_to_cpu(hdr->size_dt_struct);
    uintptr_t strings_base = dtb_va + be32_to_cpu(hdr->off_dt_strings);

    int in_memory_node = 0;
    int in_chosen_node = 0;

    while (struct_ptr < struct_end) {
        uint32_t token = be32_to_cpu(*(const uint32_t *)struct_ptr);
        struct_ptr += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)struct_ptr;
            in_memory_node = str_starts_with(name, "memory");
            in_chosen_node = str_starts_with(name, "chosen");
            uintptr_t len = 0;
            while (name[len] != '\0') len++;
            struct_ptr = align4(struct_ptr + len + 1);
        } else if (token == FDT_PROP) {
            uint32_t len     = be32_to_cpu(*(const uint32_t *)struct_ptr);
            uint32_t nameoff = be32_to_cpu(*(const uint32_t *)(struct_ptr + 4));
            struct_ptr += 8;

            const uint32_t *cells = (const uint32_t *)struct_ptr;
            const char *prop_name = (const char *)(strings_base + nameoff);

            if (in_memory_node && str_eq(prop_name, "reg") && len >= 16) {
                uint64_t base = ((uint64_t)be32_to_cpu(cells[0]) << 32) | be32_to_cpu(cells[1]);
                uint64_t size = ((uint64_t)be32_to_cpu(cells[2]) << 32) | be32_to_cpu(cells[3]);
                out_info->ram_base = base;
                out_info->ram_size = size;
            } else if (in_chosen_node && str_eq(prop_name, "linux,initrd-start")) {
                if (len >= 8) {
                    out_info->initrd_start = ((uint64_t)be32_to_cpu(cells[0]) << 32) | be32_to_cpu(cells[1]);
                } else if (len == 4) {
                    out_info->initrd_start = (uint64_t)be32_to_cpu(cells[0]);
                }
            } else if (in_chosen_node && str_eq(prop_name, "linux,initrd-end")) {
                if (len >= 8) {
                    out_info->initrd_end = ((uint64_t)be32_to_cpu(cells[0]) << 32) | be32_to_cpu(cells[1]);
                } else if (len == 4) {
                    out_info->initrd_end = (uint64_t)be32_to_cpu(cells[0]);
                }
            }
            struct_ptr = align4(struct_ptr + len);
        } else if (token == FDT_END_NODE) {
            in_memory_node = 0;
            in_chosen_node = 0;
        } else if (token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            break;
        }
    }

    dprintf(INFO, "[dtb] FDT at PA %p (VA %p, %u B): RAM %p .. %p (%lu MB)\n",
            (void *)dtb_paddr, (void *)dtb_va, out_info->dtb_size,
            (void *)out_info->ram_base,
            (void *)(out_info->ram_base + out_info->ram_size),
            out_info->ram_size / (1024 * 1024));
    if (out_info->initrd_end > out_info->initrd_start) {
        dprintf(INFO, "[dtb] Initrd (BOOTFS) at PA %p .. %p (%lu B)\n",
                (void *)out_info->initrd_start, (void *)out_info->initrd_end,
                out_info->initrd_end - out_info->initrd_start);
    }
    return 0;
}
