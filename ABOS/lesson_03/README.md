# ABOS Lesson 03: Device Tree (DTB) Handoff, PMM Arena (`vm_page_t`) & Kernel Heap

## Overview
Lesson 03 implements Zircon's physical memory and kernel heap initialization stages (`vm_init_preheap()` and `heap_init()` in `zircon_org/kernel/top/main.cpp`):

1. **Raw Kernel Binary & DTB Handoff (`dtb.c`, `Makefile`)**:
   * Converts `kernel.elf` into `kernel.img` (`objcopy -O binary`) so QEMU uses the ARM64 Linux/ZBI boot protocol, placing the Device Tree Blob at `0x40000000`, loading `kernel.img` at `0x40080000`, and passing `x0 = 0x40000000`.
   * Parses the FDT `memory` node to discover the physical RAM arena base (`0x40000000`) and size (`128 MB`).
2. **Zircon-Style PMM Arena (`pmm.h`, `pmm.c`)**:
   * Models `zircon_org/kernel/vm/include/vm/page.h` and `pmm_arena.cpp`: every 4KB frame has a `vm_page_t` entry with a Zircon `vm_page_state_t` (`VM_PAGE_STATE_FREE`, `WIRED`, `HEAP`, `MMU`, `OBJECT`, `IPC`, `ALLOC`) and `pin_count`.
   * Supports O(1) single-page allocation (`pmm_alloc_page`) and contiguous multi-page allocation (`pmm_alloc_contiguous`).
3. **Kernel Heap (`heap.h`, `heap.c`)**:
   * Allocates a contiguous pool of `VM_PAGE_STATE_HEAP` pages from the PMM and provides 16-byte-aligned `kmalloc`, `kzalloc`, and coalescing `kfree` for dynamic kernel structures (`thread_t`, `Dispatcher` objects, `Handle`s, `MessagePacket`s).

## Building & Running
```bash
make
make run
```
