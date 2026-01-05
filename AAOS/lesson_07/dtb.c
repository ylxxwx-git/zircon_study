#include "dtb.h"
#include "output.h"

// Prototypes for output functions if not in output.h
extern void safe_puts(const char *s);
extern void safe_puthex(uint64_t v);

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

uint32_t be32_to_cpu(uint32_t be) {
    return ((be & 0xFF000000) >> 24) |
           ((be & 0x00FF0000) >> 8)  |
           ((be & 0x0000FF00) << 8)  |
           ((be & 0x000000FF) << 24);
}

// Simple strcmp for node names
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Align to 4 bytes
uintptr_t align_up(uintptr_t ptr, int alignment) {
    return (ptr + alignment - 1) & ~(alignment - 1);
}

uint64_t memory_base = 0;
uint64_t memory_size = 1; // Force to .data

void dtb_init(uint64_t dtb_ptr) {
    struct fdt_header *header = (struct fdt_header *)dtb_ptr;
    
    if (be32_to_cpu(header->magic) != FDT_MAGIC) {
        safe_puts("Invalid DTB Magic!\n");
        return;
    }
    
    safe_puts("DTB Magic Valid.\n");
    
    uint32_t off_struct = be32_to_cpu(header->off_dt_struct);
    // uint32_t off_strings = be32_to_cpu(header->off_dt_strings);
    
    uintptr_t struct_ptr = dtb_ptr + off_struct;
    uintptr_t end_ptr = struct_ptr + be32_to_cpu(header->size_dt_struct);
    
    int in_memory_node = 0;
    
    while (struct_ptr < end_ptr) {
        uint32_t token = be32_to_cpu(*(uint32_t *)struct_ptr);
        struct_ptr += 4;
        
        if (token == FDT_BEGIN_NODE) {
            char *name = (char *)struct_ptr;
            // safe_puts("Node: "); safe_puts(name); safe_puts("\n");
            
            if (strncmp(name, "memory", 6) == 0) {
                // safe_puts("Found Memory Node!\n");
                in_memory_node = 1;
            } else {
                in_memory_node = 0;
            }
            
            // Advance past name (aligned to 4 bytes)
            int len = 0;
            while (name[len] != '\0') len++;
            struct_ptr += align_up(len + 1, 4);
            
        } else if (token == FDT_PROP) {
            uint32_t len = be32_to_cpu(*(uint32_t *)struct_ptr);
            struct_ptr += 4;
            uint32_t nameoff = be32_to_cpu(*(uint32_t *)struct_ptr);
            struct_ptr += 4;
            
            // Property Value
            void *val = (void *)struct_ptr;
            
            if (in_memory_node) {
                // We need to check the property name.
                // The name is in the strings block at offset `nameoff`.
                char *prop_name = (char *)(dtb_ptr + be32_to_cpu(header->off_dt_strings) + nameoff);
                
                if (strcmp(prop_name, "reg") == 0) {
                    // Found 'reg' property in 'memory' node
                    // Assuming 2 cells for address and 2 cells for size (64-bit)
                    // This depends on #address-cells and #size-cells in root, but QEMU virt usually uses 2 for both.
                    
                    if (len >= 16) {
                        // uint64_t *reg_val = (uint64_t *)val;
                        // uint64_t base = be32_to_cpu(reg_val[0] >> 32) | ((uint64_t)be32_to_cpu(reg_val[0] & 0xFFFFFFFF) << 32);
                        // Wait, be32_to_cpu returns 32-bit.
                        // We need to read 2x 32-bit words for 64-bit value.
                        uint32_t *cells = (uint32_t *)val;
                        uint64_t base_addr = ((uint64_t)be32_to_cpu(cells[0]) << 32) | be32_to_cpu(cells[1]);
                        uint64_t size = ((uint64_t)be32_to_cpu(cells[2]) << 32) | be32_to_cpu(cells[3]);
                        
                        safe_puts("Memory Base: "); safe_puthex(base_addr); safe_puts("\n");
                        safe_puts("Memory Size: "); safe_puthex(size); safe_puts("\n");
                        
                        memory_base = base_addr;
                        memory_size = size;
                    }
                }
            }
            
            struct_ptr += align_up(len, 4);
        } else if (token == FDT_END_NODE) {
            in_memory_node = 0; // End of node, so we are definitely not in memory node anymore (unless nested, but memory isn't)
        } else if (token == FDT_NOP) {
            // NOP
        } else if (token == FDT_END) {
            break;
        }
    }
}
