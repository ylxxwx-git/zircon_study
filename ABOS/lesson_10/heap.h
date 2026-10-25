/* =========================================================================
 * heap.h - Kernel Heap Allocator (kmalloc / kzalloc / kfree)
 * References:
 *   - zircon_org/kernel/lib/heap/include/lib/heap.h
 *   - zircon_org/kernel/lib/heap/cmpctmalloc/cmpctmalloc.c
 * ========================================================================= */
#ifndef ABOS_HEAP_H
#define ABOS_HEAP_H

#include <stddef.h>
#include <stdint.h>

void heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);
void heap_dump_stats(void);

#endif /* ABOS_HEAP_H */
