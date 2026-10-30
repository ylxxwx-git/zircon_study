# 9 Memory Management (PMM, Heap and MMU)

In a capability-based microkernel like Zircon or ABOS, memory management has a remarkably clean separation of concerns: policy-rich abstractions such as Virtual Memory Objects (`ab_vmo_dispatcher_t`), Virtual Memory Address Regions (`ab_vmar_dispatcher_t`), and lazy demand-paging fault handlers live in the object layer (Chapter 10), while `kernel/mm/` provides the **three-tier hardware and allocation substrate** beneath them.

Every file in `kernel/mm/` exploits the foundational architectural decision made in Chapter 6 (`start.S` and `linker.ld`): because the entire 1 GB physical RAM window `[0x40000000 .. 0x80000000)` is permanently mapped into Higher-Half `TTBR1_EL1` virtual space (`Physmap` at `0xFFFFFFFF80000000 .. 0xFFFFFFFFBFFFFFFF`), converting between any physical RAM frame and a kernel-dereferenceable pointer (`paddr_to_physmap` / `physmap_to_paddr`) costs a single 64-bit constant addition or subtraction. Zero temporary `kmap()` windows or recursive page-table slots are ever needed!

This chapter examines all three files of `kernel/mm/` line by line (655 lines total):
1. **`kernel/mm/pmm.c`** (*Program 9-1*, 229 lines): Manages the 32,768 physical 4 KB frames of the 128 MB RAM arena (`vm_page_t`), tracking explicit Zircon-style ownership states (`FREE`, `WIRED`, `HEAP`, `MMU`, `OBJECT`, `ALLOC`, `IPC`), serving O(1) single-page and contiguous multi-page allocations, scrubbing pages outside the lock, and reclaiming `bootfs.tar`'s bootloader pages.
2. **`kernel/mm/heap.c`** (*Program 9-2*, 131 lines): Carves a 512 KB contiguous `Physmap` pool into 16-byte-aligned variable-size kernel heap blocks (`kmalloc`, `kzalloc`, `kfree`) with `0xAB054EA9` canary validation, block splitting, and single-pass multi-block coalescing.
3. **`kernel/mm/mmu.c`** (*Program 9-3*, 295 lines): Implements per-process 3-level (`L1 -> L2 -> L3`) `TTBR0_EL1` page tables (`vm_aspace_t`), hardware ASID management (`1..255`), Inner-Shareable SMP TLB shootdowns (`tlbi vae1is` / `tlbi aside1is`), and Harvard D/I-cache coherence (`dc cvau` + `ic ialluis`).

---

## 9.1 Main Functionalities

### Architecture of the Three-Tier Memory Subsystem

```text
  +-------------------------------------------------------------------------+
  |       Kernel Object & Task Layer (Chapter 10: object.c, thread.c)       |
  |  VMO / VMAR Dispatchers  |  Process / Channel / Port  |  8KB K-Stacks   |
  +--------------------------+----------------------------+-----------------+
               |                            |                      |
               | pmm_alloc_page(OBJECT)     | kzalloc / kfree      |
               | vmm_map_page / unmap       v                      |
               |             +----------------------------+        |
               |             |   Tier 2: Kernel Heap      |        |
               |             |     (kernel/mm/heap.c)     |        |
               |             |  512 KB First-Fit + Canary |        |
               |             +----------------------------+        |
               v                            | 128 contig pages     | 2 contig
  +--------------------------+              | (STATE_HEAP)         | (ALLOC)
  |  Tier 3: User VmAspace   |              |                      |
  |    (kernel/mm/mmu.c)     |              |                      |
  |  TTBR0 L1->L2->L3 + ASID |              |                      |
  +--------------------------+              |                      |
               | pmm_alloc_page(STATE_MMU)  |                      |
               v                            v                      v
  +-------------------------------------------------------------------------+
  |               Tier 1: Physical Memory Manager (pmm.c)                   |
  |     page_array[32768] (128 MB Arena) + Higher-Half Physmap Zeroing      |
  +-------------------------------------------------------------------------+
```

### Physical Page State Accounting Across ABOS

Modeling Zircon's `k pmm` diagnostic accounting (`vm_page_state_t` in `include/pmm.h`), every single 4 KB page frame in physical RAM belongs to exactly one state at any instant:

| `vm_page_state_t` | Allocated By | Backing Purpose | Freed When |
| :--- | :--- | :--- | :--- |
| `VM_PAGE_STATE_FREE` | `pmm_init` / `pmm_free_*` | Available on `free_list_head` | Popped on next allocation |
| `VM_PAGE_STATE_WIRED` | `pmm_init` | Kernel image/stacks + FDT (+ initrd until `lk_main` line 102) | Never (except `pmm_free_wired_range` on initrd!) |
| `VM_PAGE_STATE_HEAP` | `heap_init` (`128` pages) | 512 KB kernel `kmalloc`/`kzalloc` pool | Permanent kernel pool |
| `VM_PAGE_STATE_ALLOC` | `thread_create` (`2` pages) | 8 KB per-thread EL1 kernel stack | `thread_reap_dead()` on detached `THREAD_DEATH` |
| `VM_PAGE_STATE_MMU` | `vmm_create_user_aspace` / `vmm_map_page` | Per-process `TTBR0_EL1` L1, L2, and L3 page-table frames | `vmm_destroy_aspace()` on process destruction |
| `VM_PAGE_STATE_OBJECT` | `ab_vmo_*` / `user_thread_trampoline` | User VMO code/data/DMA frames & 24 KB EL0 user stacks | `vmo_destroy()` / `process_destroy()` |
| `VM_PAGE_STATE_IPC` | Reserved | Zero-copy page-loaned IPC payloads | On IPC consumption |

### Dual-TTBR Virtual Address Space Map (`TCR_EL1.T0SZ = 33, T1SZ = 33`)

With `T0SZ = 33` and `T1SZ = 33`, ARMv8 splits the 64-bit virtual address space into a **2 GB per-process lower window** (`TTBR0_EL1`, ASID-tagged) and a **2 GB global kernel upper window** (`TTBR1_EL1`):

| Virtual Address Range | Active Table | Size | Granularity | Permissions & Role |
| :--- | :--- | :---: | :---: | :--- |
| `0x00000000_00000000 .. 00000FFF` | `TTBR0_EL1` | 4 KB | Unmapped | **NULL Guard Page** (`USER_ASPACE_BASE = 0x1000`) |
| `0x00000000_00083000 .. 0009xxxx` | `TTBR0_EL1` | ~64 KB | 4 KB (L3) | Mapped `USER\|RO\|EXEC` **only** in `userboot` |
| `0x00000000_00800000 .. 00805FFF` | `TTBR0_EL1` | 24 KB | 4 KB (L3) | Default EL0 User Stack (`6 × 4 KB`, `USER\|RW\|UXN`) |
| `0x00000000_10000000 .. 3FFFFFFF` | `TTBR0_EL1` | 768 MB | 4 KB (L3) | Root VMAR dynamic/fixed mappings (`0x20000000` `.bin` base!) |
| `0x00000000_40000000 .. 7FFFFFFF` | `TTBR0_EL1` | 1 GB | 4 KB (L3) | Upper half of `USER_ASPACE_SIZE` (`l1[1]`) |
| `0xFFFFFFFF_80000000 .. BFFFFFFF` | `TTBR1_EL1` | 1 GB | 1 GB Block | **Kernel `Physmap`** (`PA 0x40000000..0x7FFFFFFF`, Normal WBWA) |
| `0xFFFFFFFF_C0000000 .. FFFFFFFF` | `TTBR1_EL1` | 1 GB | 1 GB Block | **Kernel `Periphmap`** (`PA 0x00000000..0x3FFFFFFF`, Device-nGnRnE) |

---

## 9.2 `kernel/mm/pmm.c`

### 9.2.1 Functional Description

`kernel/mm/pmm.c` manages physical RAM at `4096`-byte page granularity using a flat metadata array `vm_page_t page_array[PMM_MAX_PAGES]` (`32,768` entries covering `128 MB` from `arena_base = 0x40000000` to `0x48000000`).

Examine the compact 16-byte layout of `vm_page_t` (defined in `include/pmm.h`):

```text
  struct vm_page (16 bytes total per 4 KB physical frame -> 0.39% RAM overhead!)
  +-----------------------------------+-------------------+-------+-----------+-------+
  |            paddr (8 B)            | next_free_idx(4B) | state | pin_count | flags |
  |     0x00000000_40000000 + i*4K    |  0..32767 or -1U  | (1 B) |   (1 B)   | (2 B) |
  +-----------------------------------+-------------------+-------+-----------+-------+
```

Because `next_free_idx` threads the free list directly through `page_array[]` using 32-bit indices:
- **`pmm_alloc_page()`** pops `free_list_head` in **O(1)** constant time (~10 instructions under `pmm_lock`), drops `pmm_lock`, and zeroes the frame through `Physmap`.
- **`pmm_free_page()`** computes `idx = (paddr - arena_base) / PAGE_SIZE` in **O(1)** constant time, guards against double-free and wired-page corruption, and pushes `idx` onto `free_list_head`.
- **`pmm_alloc_contiguous(count)`** scans `page_array[]` for `count` adjacent `FREE` frames, marks them allocated, rebuilds `free_list_head` cleanly, and zeroes all `count` frames outside `pmm_lock`.

### 9.2.2 Heavily Commented Source Code
#### Program 9-1: `lesson_20/kernel/mm/pmm.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/mm/pmm.c" start="1" end="229" total="229" -->
```c
       //// kernel/mm/pmm.c implements ABOS's Physical Memory Manager (PMM), modeled
       //// directly after Google Zircon's PmmArena and vm_page_t page-frame database
       //// (zircon/kernel/vm/pmm_arena.cc). Every 4 KB physical page frame in DRAM is
       //// represented by a 16-byte vm_page_t descriptor in page_array[] and classified
       //// into one of seven explicit ownership states so not a single page of RAM goes
       //// unaccounted for.
   1 | /* =========================================================================
   2 |  * pmm.c - Zircon-Style PMM Using Higher-Half Physmap
   3 |  * ========================================================================= */
   4 | #include "pmm.h"
   5 | #include "mmu.h"
   6 | #include "spinlock.h"
   7 | #include "debuglog.h"
   8 | 
       //// PMM arena globals (residing in .bss, protected by IRQ-safe pmm_lock):
       //// - PMM_INVALID_IDX (0xFFFFFFFFU): Sentinel terminating the index-based singly linked free list (analogous to NULL, using 32-bit array indices instead of 64-bit pointers to cut vm_page_t size in half!).
       //// - arena_base: Base physical address of DRAM discovered via DTB (0x40000000 on QEMU 'virt').
       //// - arena_num_pages: Total 4 KB frames managed (32,768 pages for 128 MB RAM).
       //// - free_list_head / free_page_count: Head index and live length of the O(1) free-page stack.
       //// - page_array[PMM_MAX_PAGES]: Static metadata array (32,768 * 16 bytes = 512 KB in .bss).
   9 | #define PMM_INVALID_IDX 0xFFFFFFFFU
  10 | 
  11 | static spin_lock_t pmm_lock = SPIN_LOCK_INITIAL_VALUE;
  12 | static paddr_t arena_base = 0;
  13 | static uint32_t arena_num_pages = 0;
  14 | static uint32_t free_list_head = PMM_INVALID_IDX;
  15 | static uint32_t free_page_count = 0;
  16 | static vm_page_t page_array[PMM_MAX_PAGES];
  17 | 
       //// vm_page_state_name(): Maps each vm_page_state_t enum constant (include/pmm.h)
       //// to a human-readable label for diagnostics:
       //// - FREE:   Unallocated frame on free_list_head.
       //// - ALLOC:  General kernel contiguous allocations (8 KB kernel thread stacks in thread.c).
       //// - OBJECT: Backing pages of Virtual Memory Objects (VMOs: bootfs.tar, ELF/bin code, user stacks, VirtIO DMA buffers).
       //// - WIRED:  Permanently pinned boot/kernel frames ([0x40000000 .. _end), DTB, and bootfs initrd prior to reclamation).
       //// - HEAP:   Backing pages of the 512 KB kmalloc/kfree pool (heap.c).
       //// - MMU:    Hardware Stage-1 L1/L2/L3 page-table frames allocated by mmu.c.
       //// - IPC:    Reserved state tag for zero-copy IPC buffers.
  18 | const char *vm_page_state_name(vm_page_state_t state) {
  19 |     switch (state) {
  20 |         case VM_PAGE_STATE_FREE:   return "FREE";
  21 |         case VM_PAGE_STATE_ALLOC:  return "ALLOC";
  22 |         case VM_PAGE_STATE_OBJECT: return "OBJECT(VMO)";
  23 |         case VM_PAGE_STATE_WIRED:  return "WIRED";
  24 |         case VM_PAGE_STATE_HEAP:   return "HEAP";
  25 |         case VM_PAGE_STATE_MMU:    return "MMU";
  26 |         case VM_PAGE_STATE_IPC:    return "IPC";
  27 |         default:                   return "UNKNOWN";
  28 |     }
  29 | }
  30 | 
       //// zero_page_physmap(paddr_t pa): Scrubs an entire 4096-byte physical page to
       //// zero through its Higher-Half Physmap virtual address (PA + 0xFFFFFFFF40000000)
       //// using 512 back-to-back 64-bit stores. Because start.S mapped the entire 1 GB
       //// RAM arena into the Physmap with Normal WBWA caching, scrubbing a physical page
       //// requires ZERO temporary PTE mappings or TLB flushes!
  31 | static void zero_page_physmap(paddr_t pa) {
  32 |     uint64_t *p = (uint64_t *)paddr_to_physmap(pa);
  33 |     for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
  34 |         p[i] = 0;
  35 |     }
  36 | }
  37 | 
       //// pmm_init(): Initializes the physical page arena using the hardware topology
       //// discovered by dtb_parse() and the kernel's linked _end symbol. Clamps total
       //// pages at PMM_MAX_PAGES (32,768 frames = 128 MB) so page_array[] can never
       //// overflow even if QEMU is launched with '-m 512M'.
  38 | void pmm_init(paddr_t ram_base, uint64_t ram_size, paddr_t kernel_end_pa,
  39 |               paddr_t dtb_pa, uint32_t dtb_size,
  40 |               paddr_t initrd_start, paddr_t initrd_end) {
  41 |     arena_base = ram_base;
  42 |     uint64_t pages = ram_size / PAGE_SIZE;
  43 |     if (pages > PMM_MAX_PAGES) pages = PMM_MAX_PAGES;
  44 |     arena_num_pages = (uint32_t)pages;
  45 |     free_list_head  = PMM_INVALID_IDX;
  46 |     free_page_count = 0;
  47 | 
       //// Compute conservative 4 KB page-aligned boundaries for the three non-free
       //// regions in physical RAM:
       //// - aligned_kern_end: Rounds kernel_end_pa UP to the next 4 KB boundary (covering [0x40000000 .. _end), which includes the 512 KB boot gap, .idmap, .text, .rodata, .data, .bss, and the four 8 KB boot stacks).
       //// - [dtb_start .. dtb_end): Rounds dtb_pa DOWN and (dtb_pa + dtb_size) UP so a DTB straddling two pages wires both pages completely.
       //// - [rd_start .. rd_end): Rounds initrd_start DOWN and initrd_end UP to protect every byte of QEMU's '-initrd image/bootfs.tar' until lk_main() copies it into bootfs_vmo!
  48 |     paddr_t aligned_kern_end = (kernel_end_pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  49 |     paddr_t dtb_start = dtb_pa & ~(PAGE_SIZE - 1);
  50 |     paddr_t dtb_end   = (dtb_pa + dtb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  51 |     paddr_t rd_start  = initrd_start & ~(PAGE_SIZE - 1);
  52 |     paddr_t rd_end    = (initrd_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  53 | 
       //// Why iterate BACKWARD from (arena_num_pages - 1) down to 0?
       //// Because pushing each free frame onto the front of free_list_head
       //// ('page_array[i].next_free_idx = free_list_head; free_list_head = i;') in
       //// descending order leaves free_list_head pointing to the LOWEST free physical
       //// frame (aligned_kern_end) with next_free_idx links ascending monotonically
       //// (page k -> page k+1 -> page k+2 ...)! Early boot allocations therefore draw
       //// predictably from low physical addresses upward.
  54 |     for (int32_t i = (int32_t)arena_num_pages - 1; i >= 0; i--) {
  55 |         paddr_t pa = arena_base + (paddr_t)i * PAGE_SIZE;
  56 |         page_array[i].paddr     = pa;
  57 |         page_array[i].pin_count = 0;
  58 |         page_array[i].flags     = 0;
  59 | 
       //// Classify each physical frame i:
       //// - Any frame overlapping [0x40000000 .. aligned_kern_end), the DTB, or the initrd is marked VM_PAGE_STATE_WIRED with next_free_idx = PMM_INVALID_IDX.
       //// - Every other frame is marked VM_PAGE_STATE_FREE and prepended onto free_list_head.
  60 |         int is_dtb    = (dtb_size > 0 && pa >= dtb_start && pa < dtb_end);
  61 |         int is_initrd = (initrd_end > initrd_start && pa >= rd_start && pa < rd_end);
  62 |         if (pa < aligned_kern_end || is_dtb || is_initrd) {
  63 |             page_array[i].state         = VM_PAGE_STATE_WIRED;
  64 |             page_array[i].next_free_idx = PMM_INVALID_IDX;
  65 |         } else {
  66 |             page_array[i].state         = VM_PAGE_STATE_FREE;
  67 |             page_array[i].next_free_idx = free_list_head;
  68 |             free_list_head              = (uint32_t)i;
  69 |             free_page_count++;
  70 |         }
  71 |     }
  72 | 
       //// Print initial arena summary over UART0 (typically ~32,768 total pages:
       //// ~32,500+ FREE and ~200+ WIRED before initrd reclamation).
  73 |     dprintf(INFO, "[pmm] Arena initialized: base=%p pages=%u (%u free, %u wired)\n",
  74 |             (void *)arena_base, arena_num_pages,
  75 |             free_page_count, arena_num_pages - free_page_count);
  76 | }
  77 | 
       //// pmm_alloc_page(vm_page_state_t state, paddr_t *out_paddr): O(1) single-frame
       //// allocator used heavily by VMO demand paging (VM_PAGE_STATE_OBJECT) and MMU
       //// L1/L2/L3 page-table growth (VM_PAGE_STATE_MMU).
       //// Pops free_list_head atomically under spin_lock_irqsave(&pmm_lock, &irqstate),
       //// stamps the caller's ownership 'state', resets pin_count = 0, and decrements
       //// free_page_count.
  78 | vm_page_t *pmm_alloc_page(vm_page_state_t state, paddr_t *out_paddr) {
  79 |     spin_lock_saved_state_t irqstate;
  80 |     spin_lock_irqsave(&pmm_lock, &irqstate);
  81 | 
  82 |     if (free_list_head == PMM_INVALID_IDX) {
  83 |         spin_lock_irqrestore(&pmm_lock, irqstate);
  84 |         return NULL;
  85 |     }
  86 | 
  87 |     uint32_t idx = free_list_head;
  88 |     vm_page_t *page = &page_array[idx];
  89 |     free_list_head = page->next_free_idx;
  90 |     page->next_free_idx = PMM_INVALID_IDX;
  91 |     page->state = (uint8_t)state;
  92 |     page->pin_count = 0;
  93 |     free_page_count--;
  94 | 
       //// CRITICAL SMP SCALABILITY & SECURITY DESIGN:
       //// 1. Release pmm_lock FIRST (line 95), THEN call zero_page_physmap(page->paddr)
       //// (line 97)! Zeroing 4096 bytes takes ~64 cacheline writes; doing that outside
       //// pmm_lock keeps the IRQ-disabled critical section down to ~10 instructions so
       //// all 4 CPUs can fault in VMO pages concurrently without lock contention!
       //// 2. Zeroing EVERY freshly allocated page unconditionally inside the PMM
       //// guarantees that freed user/kernel data from an exited process can NEVER leak
       //// into a newly allocated VMO page or page table of another process!
  95 |     spin_lock_irqrestore(&pmm_lock, irqstate);
  96 | 
  97 |     zero_page_physmap(page->paddr);
  98 |     if (out_paddr) *out_paddr = page->paddr;
  99 |     return page;
 100 | }
 101 | 
       //// pmm_alloc_contiguous(size_t count, vm_page_state_t state, paddr_t *out_paddr):
       //// Physically contiguous multi-page allocator required by three callers:
       //// 1. thread_create() in thread.c: 2 contiguous pages (8 KB kernel stack).
       //// 2. heap_init() in heap.c: 128 contiguous pages (512 KB kernel heap pool).
       //// 3. ab_vmo_create_contiguous() in object.c: 32 contiguous pages (128 KB DMA
       //// buffer for user-space VirtIO-Blk 'fs-srv', since QEMU's VirtIO-Blk device
       //// lacks an IOMMU and performs DMA directly to contiguous physical addresses!).
 102 | vm_page_t *pmm_alloc_contiguous(size_t count, vm_page_state_t state, paddr_t *out_paddr) {
 103 |     if (count == 0) return NULL;
 104 |     if (count == 1) return pmm_alloc_page(state, out_paddr);
 105 | 
 106 |     spin_lock_saved_state_t irqstate;
 107 |     spin_lock_irqsave(&pmm_lock, &irqstate);
 108 | 
       //// Scan page_array[0 .. arena_num_pages - 1] linearly under pmm_lock tracking the
       //// current streak of consecutive VM_PAGE_STATE_FREE frames (run_start, run_len)
       //// until run_len == count.
 109 |     uint32_t run_start = 0, run_len = 0;
 110 |     int found = 0;
 111 | 
 112 |     for (uint32_t i = 0; i < arena_num_pages; i++) {
 113 |         if (page_array[i].state == VM_PAGE_STATE_FREE) {
 114 |             if (run_len == 0) run_start = i;
 115 |             run_len++;
 116 |             if (run_len == count) {
 117 |                 found = 1;
 118 |                 break;
 119 |             }
 120 |         } else {
 121 |             run_len = 0;
 122 |         }
 123 |     }
 124 | 
 125 |     if (!found) {
 126 |         spin_lock_irqrestore(&pmm_lock, irqstate);
 127 |         return NULL;
 128 |     }
 129 | 
       //// Commit the contiguous run [run_start .. run_start + count - 1] to 'state',
       //// then rebuild free_list_head in a clean backward sweep (lines 137-143) so all
       //// remaining FREE frames stay linked in ascending PA order without needing doubly
       //// linked list pointers inside vm_page_t!
 130 |     for (uint32_t i = run_start; i < run_start + count; i++) {
 131 |         page_array[i].state = (uint8_t)state;
 132 |         page_array[i].pin_count = 0;
 133 |         page_array[i].next_free_idx = PMM_INVALID_IDX;
 134 |         free_page_count--;
 135 |     }
 136 | 
 137 |     free_list_head = PMM_INVALID_IDX;
 138 |     for (int32_t i = (int32_t)arena_num_pages - 1; i >= 0; i--) {
 139 |         if (page_array[i].state == VM_PAGE_STATE_FREE) {
 140 |             page_array[i].next_free_idx = free_list_head;
 141 |             free_list_head = (uint32_t)i;
 142 |         }
 143 |     }
 144 | 
       //// Release pmm_lock before scrubbing all 'count' physical pages to zero via
       //// zero_page_physmap(), then return &page_array[run_start] and write the base
       //// physical address into *out_paddr.
 145 |     spin_lock_irqrestore(&pmm_lock, irqstate);
 146 | 
 147 |     for (size_t i = 0; i < count; i++) {
 148 |         zero_page_physmap(page_array[run_start + i].paddr);
 149 |     }
 150 | 
 151 |     if (out_paddr) *out_paddr = page_array[run_start].paddr;
 152 |     return &page_array[run_start];
 153 | }
 154 | 
       //// pmm_free_page(paddr_t paddr): O(1) page frame deallocator.
       //// - Computes array index directly via '(paddr - arena_base) / PAGE_SIZE' with strict bounds checks.
       //// - Notice the dual safety guard on line 164:
       ////     if (page->state != VM_PAGE_STATE_FREE && page->state != VM_PAGE_STATE_WIRED)
       ////   Ignoring FREE prevents corrupting free_list_head with a cycle on accidental double-free; ignoring WIRED ensures static kernel text/data/stacks and the FDT can NEVER be accidentally returned to the allocator!
 155 | void pmm_free_page(paddr_t paddr) {
 156 |     if (paddr < arena_base) return;
 157 |     uint32_t idx = (uint32_t)((paddr - arena_base) / PAGE_SIZE);
 158 |     if (idx >= arena_num_pages) return;
 159 | 
 160 |     spin_lock_saved_state_t irqstate;
 161 |     spin_lock_irqsave(&pmm_lock, &irqstate);
 162 | 
 163 |     vm_page_t *page = &page_array[idx];
 164 |     if (page->state != VM_PAGE_STATE_FREE && page->state != VM_PAGE_STATE_WIRED) {
 165 |         page->state = VM_PAGE_STATE_FREE;
 166 |         page->pin_count = 0;
 167 |         page->next_free_idx = free_list_head;
 168 |         free_list_head = idx;
 169 |         free_page_count++;
 170 |     }
 171 | 
 172 |     spin_lock_irqrestore(&pmm_lock, irqstate);
 173 | }
 174 | 
       //// pmm_free_wired_range(paddr_t start_pa, paddr_t end_pa): Dedicated reclamation
       //// routine for boot-time wired memory (called at line 102 of main.c right after
       //// copying bootfs.tar into bootfs_vmo).
       //// Explicitly transitions frames in [start_pa .. end_pa) whose state ==
       //// VM_PAGE_STATE_WIRED to VM_PAGE_STATE_FREE and pushes them onto free_list_head.
 175 | void pmm_free_wired_range(paddr_t start_pa, paddr_t end_pa) {
 176 |     paddr_t s = start_pa & ~(PAGE_SIZE - 1);
 177 |     paddr_t e = (end_pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 178 |     if (e <= s) return;
 179 | 
 180 |     spin_lock_saved_state_t irqstate;
 181 |     spin_lock_irqsave(&pmm_lock, &irqstate);
 182 |     for (paddr_t pa = s; pa < e; pa += PAGE_SIZE) {
 183 |         if (pa < arena_base) continue;
 184 |         uint32_t idx = (uint32_t)((pa - arena_base) / PAGE_SIZE);
 185 |         if (idx >= arena_num_pages) continue;
 186 |         vm_page_t *page = &page_array[idx];
 187 |         if (page->state == VM_PAGE_STATE_WIRED) {
 188 |             page->state         = VM_PAGE_STATE_FREE;
 189 |             page->pin_count     = 0;
 190 |             page->next_free_idx = free_list_head;
 191 |             free_list_head      = idx;
 192 |             free_page_count++;
 193 |         }
 194 |     }
 195 |     spin_lock_irqrestore(&pmm_lock, irqstate);
 196 | }
 197 | 
       //// O(1) physical-address-to-vm_page_t lookup (pmm_paddr_to_page) and lockless
       //// scalar telemetry getters (pmm_count_free_pages, pmm_count_total_pages).
 198 | vm_page_t *pmm_paddr_to_page(paddr_t paddr) {
 199 |     if (paddr < arena_base) return NULL;
 200 |     uint32_t idx = (uint32_t)((paddr - arena_base) / PAGE_SIZE);
 201 |     if (idx >= arena_num_pages) return NULL;
 202 |     return &page_array[idx];
 203 | }
 204 | 
 205 | uint64_t pmm_count_free_pages(void) {
 206 |     return free_page_count;
 207 | }
 208 | 
 209 | uint64_t pmm_count_total_pages(void) {
 210 |     return arena_num_pages;
 211 | }
 212 | 
       //// pmm_dump_state_counts(): Snapshots a complete per-state histogram across all
       //// arena_num_pages frames under pmm_lock and logs the breakdown (FREE, WIRED,
       //// HEAP, MMU, OBJECT, IPC, ALLOC) — invaluable when verifying zero memory leaks
       //// after process exit or BOOTFS unmap!
 213 | void pmm_dump_state_counts(void) {
 214 |     uint32_t counts[VM_PAGE_STATE_COUNT] = {0};
 215 |     spin_lock_saved_state_t irqstate;
 216 |     spin_lock_irqsave(&pmm_lock, &irqstate);
 217 |     for (uint32_t i = 0; i < arena_num_pages; i++) {
 218 |         if (page_array[i].state < VM_PAGE_STATE_COUNT) {
 219 |             counts[page_array[i].state]++;
 220 |         }
 221 |     }
 222 |     spin_lock_irqrestore(&pmm_lock, irqstate);
 223 | 
 224 |     dprintf(INFO, "[pmm] Page states: FREE=%u WIRED=%u HEAP=%u MMU=%u OBJECT=%u IPC=%u ALLOC=%u\n",
 225 |             counts[VM_PAGE_STATE_FREE], counts[VM_PAGE_STATE_WIRED],
 226 |             counts[VM_PAGE_STATE_HEAP], counts[VM_PAGE_STATE_MMU],
 227 |             counts[VM_PAGE_STATE_OBJECT], counts[VM_PAGE_STATE_IPC],
 228 |             counts[VM_PAGE_STATE_ALLOC]);
 229 | }
```
### 9.2.3 Technical Notes

1. **Why `pmm_free_page()` Does Not Scrub Freed Pages (Alloc-Time vs Free-Time Zeroing)**:
   Notice that `pmm_alloc_page()` and `pmm_alloc_contiguous()` scrub pages *on allocation* (`zero_page_physmap`), whereas `pmm_free_page()` simply marks the page `FREE` in O(1) time. Zeroing at allocation time has two concrete advantages: (a) process destruction (`vmm_destroy_aspace` + `vmo_destroy`) frees dozens of pages rapidly without thrashing the L1 data cache with dead zeroes, and (b) when a thread faults in a new VMO page and immediately reads/writes that page in EL0, `zero_page_physmap()` has just pulled all 64 cachelines of that physical frame warm into the CPU's L1/L2 cache!

2. **Comparison with Zircon's Per-CPU Page Caches**:
   In production Zircon (`PmmNode`), each CPU maintains a small lockless per-CPU magazine of pre-zeroed `FREE` pages (refilled in batches of 32–64 pages under the arena lock, with an idle background `page-zeroing` thread). On ABOS's 4-core system, keeping `zero_page_physmap()` *outside* `pmm_lock` already reduces `pmm_lock`'s hold time to a dozen nanoseconds, achieving virtually zero contention across all 4 cores in under 230 lines of C.

---

## 9.3 `kernel/mm/heap.c`

### 9.3.1 Functional Description

Kernel subsystems frequently allocate small, variable-sized structures whose lifetimes are independent of stack frames (`ab_process_t` ~2.5 KB, `ab_channel_dispatcher_t` ~80 B, `ab_message_packet_t` ~368 B, `thread_t` ~144 B, `vm_aspace_t` ~32 B). Handing an entire 4 KB PMM frame to a 32-byte `vm_aspace_t` would waste 99.2% of RAM!

`kernel/mm/heap.c` solves this with a classic **intrusive boundary-header first-fit allocator** operating over a 512 KB contiguous `Physmap` region (`128 × 4 KB` pages):

```text
  512 KB Contiguous Physmap Pool (VA 0xFFFFFFFF80xxxxxx)
  ======================================================
  heap_head
     |
     v
  +------------------+------------------------+------------------+--------------------+
  | heap_block_t(32B)| Allocated Payload      | heap_block_t(32B)| Free Payload       |
  | magic=0xAB054EA9 | (16-byte aligned size) | magic=0xAB054EA9 | (Coalesced space)  |
  | is_free = 0      | <-- ptr returned by    | is_free = 1      |                    |
  | next ----------->|     kmalloc()          | next = NULL      |                    |
  +------------------+------------------------+------------------+--------------------+
```

### 9.3.2 Heavily Commented Source Code
#### Program 9-2: `lesson_20/kernel/mm/heap.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/mm/heap.c" start="1" end="131" total="131" -->
```c
       //// kernel/mm/heap.c implements ABOS's dynamic kernel object allocator (kmalloc,
       //// kzalloc, kfree), providing variable-sized byte allocations backed by the
       //// Higher-Half Physmap. While large page-granular structures (VMO data pages, MMU
       //// page tables, 8 KB kernel stacks) draw directly from pmm.c, variable-sized C
       //// control structures (ab_process_t, ab_channel_dispatcher_t,
       //// ab_message_packet_t, thread_t, vm_aspace_t) are allocated here.
   1 | /* =========================================================================
   2 |  * heap.c - Kernel Heap Using Higher-Half Physmap Virtual Addresses
   3 |  * ========================================================================= */
   4 | #include "heap.h"
   5 | #include "pmm.h"
   6 | #include "mmu.h"
   7 | #include "spinlock.h"
   8 | #include "debuglog.h"
   9 | 
       //// Heap pool sizing and corruption-detection canary:
       //// - HEAP_INITIAL_PAGES = 128U: Reserves 128 * 4096 = 524,288 bytes (512 KB) of physically and virtually contiguous Physmap RAM at boot.
       //// - HEAP_MAGIC = 0xAB054EA9U: 32-bit header canary ("ABOS HEAP") written into every free and allocated block header so kfree() immediately catches invalid pointers, buffer-underflow corruptions, or misaligned frees.
  10 | #define HEAP_INITIAL_PAGES 128U /* 512KB initial kernel heap pool */
  11 | #define HEAP_MAGIC         0xAB054EA9U
  12 | 
       //// heap_block_t: Intrusive 32-byte boundary header preceding every free or
       //// allocated payload chunk:
       ////   Offset 0  : uint32_t magic    (4 B = 0xAB054EA9)
       ////   Offset 4  : uint32_t is_free  (4 B = 1 free, 0 allocated)
       ////   Offset 8  : size_t   size     (8 B = usable payload bytes AFTER this header)
       ////   Offset 16 : heap_block_t *next(8 B = next block in ascending address order)
       ////   Offset 24 : [8 B tail padding due to __attribute__((aligned(16)))] -> sizeof = 32 B!
       //// Because sizeof(heap_block_t) == 32 is an exact multiple of 16, if heap_block_t
       //// starts at a 16-byte-aligned address, then (curr + 1) — the payload pointer
       //// returned to kmalloc's caller — is ALSO guaranteed to be 16-byte aligned!
  13 | typedef struct heap_block {
  14 |     uint32_t magic;
  15 |     uint32_t is_free;
  16 |     size_t   size;
  17 |     struct heap_block *next;
  18 | } __attribute__((aligned(16))) heap_block_t;
  19 | 
       //// Module-global heap state serialized across all 4 SMP cores and IRQ contexts by
       //// spin_lock_irqsave(&heap_lock, ...).
  20 | static spin_lock_t heap_lock = SPIN_LOCK_INITIAL_VALUE;
  21 | static heap_block_t *heap_head = NULL;
  22 | static size_t total_heap_bytes = 0;
  23 | 
       //// heap_init(): Called once by CPU 0 in lk_main() immediately after pmm_init().
       //// Requests 128 contiguous pages tagged VM_PAGE_STATE_HEAP from PMM, converts
       //// base_pa to Higher-Half virtual pointer heap_head via paddr_to_physmap(), and
       //// formats the entire 512 KB arena as a single giant free block with payload
       //// capacity (524,288 - 32) = 524,256 bytes.
  24 | void heap_init(void) {
  25 |     paddr_t base_pa = 0;
  26 |     vm_page_t *pages = pmm_alloc_contiguous(HEAP_INITIAL_PAGES, VM_PAGE_STATE_HEAP, &base_pa);
  27 |     if (!pages) {
  28 |         dprintf(CRITICAL, "[heap] PANIC: failed to allocate pages for kernel heap\n");
  29 |         for (;;) __asm__ volatile("wfe");
  30 |     }
  31 | 
  32 |     total_heap_bytes = HEAP_INITIAL_PAGES * PAGE_SIZE;
  33 |     heap_head = (heap_block_t *)paddr_to_physmap(base_pa);
  34 |     heap_head->magic   = HEAP_MAGIC;
  35 |     heap_head->is_free = 1;
  36 |     heap_head->size    = total_heap_bytes - sizeof(heap_block_t);
  37 |     heap_head->next    = NULL;
  38 | 
  39 |     dprintf(INFO, "[heap] Initialized %u KB kernel heap at VA %p (PA %p)\n",
  40 |             (uint32_t)(total_heap_bytes / 1024), (void *)heap_head, (void *)base_pa);
  41 | }
  42 | 
       //// kmalloc(size_t size): First-fit SMP-safe kernel heap allocator.
       //// Line 45 rounds 'size' UP to a multiple of 16 bytes ('(size + 15UL) & ~15UL').
       //// On AArch64, 16-byte payload alignment is mandatory so compiler-generated
       //// 128-bit load/store-pair instructions and atomic operations inside allocated
       //// structs always remain naturally aligned, and so any subsequent split header
       //// placed at '(uint8_t *)(curr + 1) + size' also lands on a 16-byte boundary!
  43 | void *kmalloc(size_t size) {
  44 |     if (size == 0) return NULL;
  45 |     size = (size + 15UL) & ~15UL;
  46 | 
  47 |     spin_lock_saved_state_t irqstate;
  48 |     spin_lock_irqsave(&heap_lock, &irqstate);
  49 | 
       //// First-fit scan & block splitting under heap_lock:
       //// When a free block 'curr' has curr->size >= size, line 53 checks whether the
       //// leftover remainder is large enough to justify carving a new free block:
       ////   curr->size >= size + sizeof(heap_block_t) + 32UL
       //// Requiring at least 32 payload bytes (plus the 32-byte header = 64 excess
       //// bytes) avoids polluting the linked list with tiny unusable 16-byte splinter
       //// blocks. When splitting, 'split' is initialized right after curr's shrunk
       //// payload and spliced into curr->next.
  50 |     heap_block_t *curr = heap_head;
  51 |     while (curr) {
  52 |         if (curr->is_free && curr->size >= size) {
  53 |             if (curr->size >= size + sizeof(heap_block_t) + 32UL) {
  54 |                 heap_block_t *split = (heap_block_t *)((uint8_t *)(curr + 1) + size);
  55 |                 split->magic   = HEAP_MAGIC;
  56 |                 split->is_free = 1;
  57 |                 split->size    = curr->size - size - sizeof(heap_block_t);
  58 |                 split->next    = curr->next;
  59 | 
  60 |                 curr->size = size;
  61 |                 curr->next = split;
  62 |             }
       //// Mark 'curr' allocated (is_free = 0), release heap_lock, and return '(void
       //// *)(curr + 1)' — pointing directly to the first byte after the 32-byte
       //// heap_block_t header.
  63 |             curr->is_free = 0;
  64 |             spin_lock_irqrestore(&heap_lock, irqstate);
  65 |             return (void *)(curr + 1);
  66 |         }
  67 |         curr = curr->next;
  68 |     }
  69 | 
  70 |     spin_lock_irqrestore(&heap_lock, irqstate);
  71 |     return NULL;
  72 | }
  73 | 
       //// kzalloc(size_t size): Convenience wrapper allocating via kmalloc(size) and
       //// zero-filling all 'size' bytes. Almost every kernel Dispatcher constructor
       //// (ab_object_alloc, thread_create, vmm_create_user_aspace) calls kzalloc() so
       //// newly created kernel objects start in a deterministic all-zero state.
  74 | void *kzalloc(size_t size) {
  75 |     void *p = kmalloc(size);
  76 |     if (p) {
  77 |         uint8_t *b = (uint8_t *)p;
  78 |         for (size_t i = 0; i < size; i++) b[i] = 0;
  79 |     }
  80 |     return p;
  81 | }
  82 | 
       //// kfree(void *ptr): Frees a previously allocated payload pointer and coalesces
       //// adjacent free blocks.
       //// 1. C-standard NULL tolerance: kfree(NULL) is a safe no-op.
       //// 2. Steps backward 32 bytes ('((heap_block_t *)ptr) - 1') to recover the
       //// block's heap_block_t header and validates blk->magic == HEAP_MAGIC BEFORE
       //// modifying heap state! If magic is corrupted, unlocks heap_lock and logs a
       //// CRITICAL error rather than corrupting the allocator list.
  83 | void kfree(void *ptr) {
  84 |     if (!ptr) return;
  85 | 
  86 |     spin_lock_saved_state_t irqstate;
  87 |     spin_lock_irqsave(&heap_lock, &irqstate);
  88 | 
  89 |     heap_block_t *blk = ((heap_block_t *)ptr) - 1;
  90 |     if (blk->magic != HEAP_MAGIC) {
  91 |         spin_lock_irqrestore(&heap_lock, irqstate);
  92 |         dprintf(CRITICAL, "[heap] Invalid kfree(%p) - bad magic!\n", ptr);
  93 |         return;
  94 |     }
  95 |     blk->is_free = 1;
  96 | 
       //// Single-pass multi-block forward coalescing loop:
       //// Walks from heap_head merging any pair of consecutive free blocks
       //// (curr->is_free && curr->next->is_free) that are physically adjacent in memory
       //// ('(uintptr_t)curr->next == expected_next'):
       ////   [curr hdr | curr payload][curr->next hdr | next payload]
       ////               becomes:
       ////   [curr hdr | curr->size + 32 + next->size               ]
       //// LOOK CLOSELY at lines 102-106: when a merge happens, we do NOT execute 'curr =
       //// curr->next'! Keeping 'curr' on the newly expanded block allows the very next
       //// while-loop iteration to merge 'curr' with the FOLLOWING free block as well —
       //// collapsing [Free A][Newly Freed B][Free C] into a single unified free block in
       //// one linear pass!
  97 |     heap_block_t *curr = heap_head;
  98 |     while (curr && curr->next) {
  99 |         uintptr_t expected_next = (uintptr_t)(curr + 1) + curr->size;
 100 |         if (curr->is_free && curr->next->is_free &&
 101 |             (uintptr_t)curr->next == expected_next) {
 102 |             curr->size += sizeof(heap_block_t) + curr->next->size;
 103 |             curr->next  = curr->next->next;
 104 |         } else {
 105 |             curr = curr->next;
 106 |         }
 107 |     }
 108 | 
 109 |     spin_lock_irqrestore(&heap_lock, irqstate);
 110 | }
 111 | 
       //// heap_dump_stats(): Iterates the block list under heap_lock to tally live
       //// used_bytes/used_blks and free_bytes/free_blks for runtime memory-leak
       //// auditing.
 112 | void heap_dump_stats(void) {
 113 |     size_t used_bytes = 0, free_bytes = 0, used_blks = 0, free_blks = 0;
 114 | 
 115 |     spin_lock_saved_state_t irqstate;
 116 |     spin_lock_irqsave(&heap_lock, &irqstate);
 117 |     for (heap_block_t *c = heap_head; c != NULL; c = c->next) {
 118 |         if (c->is_free) {
 119 |             free_bytes += c->size;
 120 |             free_blks++;
 121 |         } else {
 122 |             used_bytes += c->size;
 123 |             used_blks++;
 124 |         }
 125 |     }
 126 |     spin_lock_irqrestore(&heap_lock, irqstate);
 127 | 
 128 |     dprintf(INFO, "[heap] Stats: used=%lu B (%lu blks), free=%lu B (%lu blks)\n",
 129 |             (unsigned long)used_bytes, (unsigned long)used_blks,
 130 |             (unsigned long)free_bytes, (unsigned long)free_blks);
 131 | }
```
### 9.3.3 Technical Notes

1. **Why Single-Pass Coalescing Works Regardless of Free Order**:
   Suppose three adjacent blocks `[A][B][C]` are allocated, and the kernel calls `kfree(A)`, then `kfree(C)`, and finally `kfree(B)`:
   - After `kfree(A)` and `kfree(C)`, `A` and `C` are separated by allocated `B`, so neither merges yet.
   - When `kfree(B)` runs, `B->is_free` becomes `1`, and the coalescing scan starts from `heap_head`.
   - When `curr == A`, `A->is_free && B->is_free` holds! Lines 102–103 absorb `B` into `A` (`A->size += 32 + B->size; A->next = C;`) **without advancing `curr`**.
   - On the very next loop iteration, `curr` is *still* `A`, and `curr->next` is now `C`! Since `A->is_free && C->is_free` also holds and `(uintptr_t)C == (uintptr_t)(A + 1) + A->size`, `A` immediately absorbs `C` as well — restoring a single completely unfragmented free block!

2. **Comparison with Zircon's `cmpctmalloc` (`zircon/kernel/lib/heap/cmpctmalloc`)**:
   Zircon's compact kernel malloc (`cmpctmalloc`) maintains segregated free lists across geometric size buckets (`32 B, 40 B, 48 B ...`) with O(1) constant-time doubly linked boundary-tag coalescing and dynamic arena growth/shrinking back to the PMM. ABOS's 131-line linear first-fit allocator typically holds fewer than 40–80 live kernel object blocks at once, making list traversal take under a microsecond while keeping every invariant immediately transparent to the reader.

---

## 9.4 `kernel/mm/mmu.c`

### 9.4.1 Functional Description

`kernel/mm/mmu.c` implements user-space virtual memory isolation. Every EL0 process (`userboot`, `console-srv`, `fs-srv`, `shell-0..3`, `calc`) owns a dedicated `vm_aspace_t` instance paired with a unique hardware **ASID (`1..255`)** and its own 3-level Stage-1 page-table hierarchy rooted at `ttbr0_phys`:

```text
  TTBR0_EL1 = [ ASID (bits 63:48) | ttbr0_phys (bits 47:12) ]
                                          |
                                          v
                        +-----------------------------------+
                        | Level-1 Table (4 KB, VM_PAGE_MMU) |
                        |  [0]: 0x0000_0000 .. 0x3FFF_FFFF  | ---> L2 Table (4 KB)
                        |  [1]: 0x4000_0000 .. 0x7FFF_FFFF  |        |
                        |  [2..511]: Unused (T0SZ = 33)     |        | l2_idx = va[29:21]
                        +-----------------------------------+        v
                                                            +--------------------+
                                                            | L3 Table (4 KB)    |
                                                            |  l3_idx = va[20:12]|
                                                            +--------------------+
                                                                     |
                                                                     v
                                                            4 KB Physical Frame
                                                            (VMO / Stack / MMIO)
```

Study the hardware permission matrix produced by `flags_to_pte()` (lines 104–132) for every EL0 mapping:

| Mapping Kind in ABOS | `MMU_FLAG_*` Passed | `AttrIndx` | `AP[2:1]` | `PXN` | `UXN` | `nG` |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| User Code (`.text`, `bootfs` exec) | `USER \| READ \| EXEC` | Normal (`1`) | `0b11` (EL0 RO) | **`1`** | `0` | **`1`** |
| Read-Only VMO (`bootfs.tar` @ `0x10000000`) | `USER \| READ` | Normal (`1`) | `0b11` (EL0 RO) | **`1`** | **`1`** | **`1`** |
| User Stack / `.data` / `.bss` / DMA VMO | `USER \| READ \| WRITE` | Normal (`1`) | `0b01` (EL0 RW) | **`1`** | **`1`** | **`1`** |
| Driver MMIO (`console-srv` UART, `fs-srv` VirtIO) | `USER \| READ \| WRITE \| DEVICE` | Device (`0`) | `0b01` (EL0 RW) | **`1`** | **`1`** | **`1`** |

Notice two rock-solid security invariants across every row of the table:
1. **`PXN = 1` on 100% of user pages**: Privileged EL1 mode can **never** execute instructions from any `TTBR0_EL1` user page.
2. **Strict `W^X` in practice**: User stacks, heaps, DMA buffers, and MMIO regions always have `UXN = 1`, while executable code segments (`userboot` and read-only segments) omit write permissions unless explicitly requested by a loader stage (and whenever `EXEC` is mapped, `sync_icache_page()` cleans L1D to PoU and invalidates L1I across all 4 SMP cores!).

### 9.4.2 Heavily Commented Source Code
#### Program 9-3: `lesson_20/kernel/mm/mmu.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/mm/mmu.c" start="1" end="295" total="295" -->
```c
       //// kernel/mm/mmu.c implements ABOS's per-process Virtual Memory Address Space
       //// manager (vm_aspace_t, modeled after Zircon's VmAspace / ArchVmAspace in
       //// zircon/kernel/arch/arm64/mmu.cpp). While TTBR1_EL1 permanently holds start.S's
       //// 1 GB Higher-Half kernel_table across all CPUs, TTBR0_EL1 is switched per
       //// process by mmu.c using 3-level (L1 -> L2 -> L3) 4 KB granule page tables
       //// tagged with hardware Address Space Identifiers (ASIDs).
   1 | /* =========================================================================
   2 |  * mmu.c - Zircon-Style VmAspace & 3-Level (L1->L2->L3) Page Table Management
   3 |  * References:
   4 |  *   - zircon_org/kernel/arch/arm64/mmu.cpp
   5 |  *   - zircon_org/kernel/vm/vm_aspace.cpp
   6 |  * ========================================================================= */
   7 | #include "mmu.h"
   8 | #include "heap.h"
   9 | #include "debuglog.h"
  10 | 
       //// empty_ttbr0_l1[512]: A permanently all-zero, 4 KB-aligned Level-1 page table
       //// in .bss. Whenever a CPU runs an idle thread or pure kernel thread
       //// (thread->aspace == NULL), vmm_switch_aspace(NULL) points TTBR0_EL1 at
       //// empty_ttbr0_l1 (with ASID 0). Because all 512 entries of empty_ttbr0_l1 have
       //// PTE_VALID == 0:
       //// 1. It cleanly retires start.S's temporary physical trampoline (idmap_table).
       //// 2. Any accidental NULL-pointer or low-address dereference in kernel thread
       //// context immediately triggers a deterministic Level-1 Translation Fault rather
       //// than silently reading stale user or physical memory!
  11 | static pte_t empty_ttbr0_l1[512] __attribute__((aligned(4096)));
  12 | 
       //// Hardware ASID allocator table (MAX_ASIDS = 256, fitting within both 8-bit and
       //// 16-bit ARMv8 ASID implementations), protected by IRQ-safe g_asid_lock.
  13 | #define MAX_ASIDS 256U
  14 | static spin_lock_t g_asid_lock = SPIN_LOCK_INITIAL_VALUE;
  15 | static uint8_t g_asid_used[MAX_ASIDS];
  16 | 
       //// tlb_flush_page(uint16_t asid, vaddr_t vaddr): Broadcasts a surgical
       //// single-page, single-ASID TLB shootdown across ALL 4 SMP cores!
       //// Decode the 64-bit register operand for 'tlbi vae1is, %0' (TLB Invalidate by
       //// VA, EL1&0 regime, Inner Shareable):
       //// - Bits [63:48]: 16-bit ASID (zero-extended from uint16_t asid) — leaves TLB entries of all other processes untouched!
       //// - Bits [43:0]:  Virtual Page Number (vaddr >> 12).
       //// And note the mandatory 4-instruction ARMv8 SMP barrier sequence:
       //// 1. 'dsb ishst': Waits for our prior PTE store into RAM to become visible to
       //// hardware page-table walkers on all Inner-Shareable cores BEFORE invalidating
       //// stale TLBs.
       //// 2. 'tlbi vae1is, %0': Broadcasts the TLB invalidation message across the
       //// Inner-Shareable interconnect to CPUs 0..3.
       //// 3. 'dsb ish': Waits until every remote CPU core acknowledges completion of the
       //// TLB invalidation!
       //// 4. 'isb': Flushes the local CPU's instruction fetch pipeline so subsequent
       //// instructions see the new translation.
  17 | static inline void tlb_flush_page(uint16_t asid, vaddr_t vaddr) {
  18 |     uint64_t operand = ((uint64_t)asid << 48) | ((vaddr >> 12) & 0x00000FFFFFFFFFFFULL);
  19 |     __asm__ volatile(
  20 |         "dsb ishst\n"
  21 |         "tlbi vae1is, %0\n"
  22 |         "dsb ish\n"
  23 |         "isb\n"
  24 |         :: "r"(operand) : "memory");
  25 | }
  26 | 
       //// tlb_flush_asid(uint16_t asid): Broadcasts an Inner-Shareable TLB invalidation
       //// of ALL virtual pages tagged with 'asid' ('tlbi aside1is, %0') across all 4
       //// cores. Invoked both when allocating an ASID and when freeing an ASID on
       //// process destruction.
  27 | static inline void tlb_flush_asid(uint16_t asid) {
  28 |     uint64_t operand = (uint64_t)asid << 48;
  29 |     __asm__ volatile(
  30 |         "dsb ishst\n"
  31 |         "tlbi aside1is, %0\n"
  32 |         "dsb ish\n"
  33 |         "isb\n"
  34 |         :: "r"(operand) : "memory");
  35 | }
  36 | 
       //// sync_icache_page(paddr_t paddr): Resolves the ARMv8 Harvard Cache Coherence
       //// hazard whenever executable code is mapped into EL0!
       //// Why is this function critical when userboot loads console-srv.bin / shell.bin
       //// or shell loads calc.bin?
       //// ARMv8-A has separate L1 Data and L1 Instruction caches that are NOT
       //// hardware-coherent with each other: when the kernel/loader writes program bytes
       //// into a VMO page via Physmap, those new machine instructions sit dirty in the
       //// L1 Data Cache while the L1 Instruction Cache on any of the 4 CPUs may still
       //// hold stale zeroes or old instructions from a previously freed process!
       //// - Lines 39-41 step across all 64 cachelines (4096 / 64 = 64 iterations) executing 'dc cvau' (Data Cache Clean by VA to Point of Unification), pushing dirty instruction bytes out of L1D to unified L2/RAM.
       //// - 'dsb ish' waits for all 64 cache cleans to finish globally.
       //// - 'ic ialluis' broadcasts an Inner-Shareable Instruction Cache invalidation across ALL 4 CPUs!
       //// - 'dsb ish' + 'isb' guarantees every core fetches fresh instructions from PoU.
  37 | static inline void sync_icache_page(paddr_t paddr) {
  38 |     uintptr_t kva = (uintptr_t)paddr_to_physmap(paddr);
  39 |     for (size_t off = 0; off < PAGE_SIZE; off += 64) {
  40 |         __asm__ volatile("dc cvau, %0" :: "r"(kva + off) : "memory");
  41 |     }
  42 |     __asm__ volatile(
  43 |         "dsb ish\n"
  44 |         "ic ialluis\n"
  45 |         "dsb ish\n"
  46 |         "isb\n"
  47 |         ::: "memory");
  48 | }
  49 | 
       //// ab_alloc_asid() & ab_free_asid():
       //// - ASID 0 is reserved permanently for kernel/idle empty_ttbr0_l1 (line 75).
       //// - User processes receive unique ASIDs 1..255 from g_asid_used[], calling tlb_flush_asid() on both allocation and release so recycled ASIDs never inherit stale TLB translations. (If > 255 processes run simultaneously, line 62 falls back safely to ASID 1 after flushing.)
  50 | uint16_t ab_alloc_asid(void) {
  51 |     spin_lock_saved_state_t irqstate;
  52 |     spin_lock_irqsave(&g_asid_lock, &irqstate);
  53 |     for (uint16_t asid = 1; asid < MAX_ASIDS; asid++) {
  54 |         if (!g_asid_used[asid]) {
  55 |             g_asid_used[asid] = 1;
  56 |             spin_lock_irqrestore(&g_asid_lock, irqstate);
  57 |             tlb_flush_asid(asid);
  58 |             return asid;
  59 |         }
  60 |     }
  61 |     spin_lock_irqrestore(&g_asid_lock, irqstate);
  62 |     return 1;
  63 | }
  64 | 
  65 | void ab_free_asid(uint16_t asid) {
  66 |     if (asid == 0 || asid >= MAX_ASIDS) return;
  67 |     tlb_flush_asid(asid);
  68 |     spin_lock_saved_state_t irqstate;
  69 |     spin_lock_irqsave(&g_asid_lock, &irqstate);
  70 |     g_asid_used[asid] = 0;
  71 |     spin_lock_irqrestore(&g_asid_lock, irqstate);
  72 | }
  73 | 
       //// mmu_init(): Marks ASID 0 reserved in g_asid_used[0] and logs the live hardware
       //// MMU configuration registers established by start.S (SCTLR_EL1, TCR_EL1,
       //// TTBR1_EL1).
  74 | void mmu_init(void) {
  75 |     g_asid_used[0] = 1; /* Reserve ASID 0 for kernel/idle empty TTBR0 */
  76 |     uint64_t sctlr, tcr, ttbr1;
  77 |     __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  78 |     __asm__ volatile("mrs %0, tcr_el1"   : "=r"(tcr));
  79 |     __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
  80 | 
  81 |     dprintf(INFO, "[mmu] Higher-half MMU + I/D-caches active: SCTLR_EL1=0x%lx TCR_EL1=0x%lx TTBR1_EL1=%p\n",
  82 |             sctlr, tcr, (void *)ttbr1);
  83 | }
  84 | 
       //// vmm_create_user_aspace(uint16_t asid): Constructs an isolated 2 GB EL0 virtual
       //// address space for a new process (ab_process_create_kernel).
       //// Allocates vm_aspace_t via kzalloc() and one zeroed 4 KB physical page tagged
       //// VM_PAGE_STATE_MMU for the root Level-1 page table:
       //// - ttbr0_phys stores the physical address loaded into hardware TTBR0_EL1.
       //// - l1_virt caches paddr_to_physmap(l1_pa) so software page-table walks inspect/modify L1 entries directly in Higher-Half virtual memory!
  85 | vm_aspace_t *vmm_create_user_aspace(uint16_t asid) {
  86 |     vm_aspace_t *aspace = (vm_aspace_t *)kzalloc(sizeof(vm_aspace_t));
  87 |     if (!aspace) return NULL;
  88 | 
  89 |     paddr_t l1_pa = 0;
  90 |     vm_page_t *p = pmm_alloc_page(VM_PAGE_STATE_MMU, &l1_pa);
  91 |     if (!p) {
  92 |         kfree(aspace);
  93 |         return NULL;
  94 |     }
  95 | 
  96 |     aspace->lock.value   = 0;
  97 |     aspace->ttbr0_phys   = l1_pa;
  98 |     aspace->l1_virt      = (pte_t *)paddr_to_physmap(l1_pa);
  99 |     aspace->asid         = asid;
 100 |     aspace->mapped_pages = 0;
 101 |     return aspace;
 102 | }
 103 | 
       //// flags_to_pte(uint32_t flags): Translates high-level architectural mapping
       //// flags (MMU_FLAG_*) into an AArch64 Stage-1 Level-3 4 KB Page Descriptor:
       //// - Base bits: PTE_VALID (bit 0) | PTE_PAGE (bit 1 = 1 for L3 4 KB leaf pages!) | PTE_AF (bit 10) | PTE_SH_INNER (bits [9:8] = 0b11).
       //// - Cacheability: MMU_FLAG_CACHE_DEVICE (used by sys_vmo_create_physical when mapping PL011 UART or VirtIO MMIO into driver servers!) selects MAIR AttrIdx 0 (Device-nGnRnE) and forces PXN | UXN; otherwise selects MAIR AttrIdx 1 (Normal WBWA).
       //// - Privilege & SMAP/PXN Hardening (lines 113-119): User mappings set PTE_AP_USER (EL0 accessible) and PTE_NG (Non-Global, ASID-tagged!), AND unconditionally set PTE_PXN (Privileged Execute Never) so EL1 kernel mode can NEVER accidentally execute user-space memory (defeating ret2usr attacks in hardware!). UXN is set unless MMU_FLAG_PERM_EXECUTE is explicitly requested (enforcing W^X).
       //// - Write protection: Sets PTE_AP_RO (bit 7) unless MMU_FLAG_PERM_WRITE is present.
 104 | static pte_t flags_to_pte(uint32_t flags) {
 105 |     pte_t attr = PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER;
 106 | 
 107 |     if (flags & MMU_FLAG_CACHE_DEVICE) {
 108 |         attr |= PTE_ATTR_DEVICE | PTE_PXN | PTE_UXN;
 109 |     } else {
 110 |         attr |= PTE_ATTR_NORMAL;
 111 |     }
 112 | 
 113 |     if (flags & MMU_FLAG_PERM_USER) {
 114 |         attr |= PTE_AP_USER | PTE_NG;
 115 |         /* Never allow EL1 to execute user pages (PXN) */
 116 |         attr |= PTE_PXN;
 117 |         if (!(flags & MMU_FLAG_PERM_EXECUTE)) {
 118 |             attr |= PTE_UXN;
 119 |         }
 120 |     } else {
 121 |         attr |= PTE_UXN;
 122 |         if (!(flags & MMU_FLAG_PERM_EXECUTE)) {
 123 |             attr |= PTE_PXN;
 124 |         }
 125 |     }
 126 | 
 127 |     if (!(flags & MMU_FLAG_PERM_WRITE)) {
 128 |         attr |= PTE_AP_RO;
 129 |     }
 130 | 
 131 |     return attr;
 132 | }
 133 | 
       //// vmm_map_page(): Installs or updates a single 4 KB mapping (vaddr -> paddr)
       //// inside 'aspace'.
       //// Notice the lower-bound check on line 135:
       ////   vaddr < USER_ASPACE_BASE (0x1000) || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE) (0x80000000)
       //// Rejecting vaddr < 0x1000 guarantees virtual page 0 [0x0000 .. 0x0FFF] can
       //// NEVER be mapped in any user process, ensuring NULL-pointer dereferences always
       //// fault cleanly!
 134 | int vmm_map_page(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
 135 |     if (!aspace || vaddr < USER_ASPACE_BASE || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE)) {
 136 |         return -1;
 137 |     }
 138 | 
 139 |     vaddr &= ~(PAGE_SIZE - 1);
 140 |     paddr &= ~(PAGE_SIZE - 1);
 141 | 
       //// 3-Level Page-Table Index Extraction under TCR_EL1.T0SZ = 33 (31-bit = 2 GB
       //// User VA):
       //// Because bits [63:31] must be 0 for TTBR0_EL1 when T0SZ = 33, ARMv8 skips Level
       //// 0 completely and begins hardware table walks at Level 1:
       ////   30        29       21 20       12 11         0
       ////   +--------+-----------+-----------+------------+
       ////   | L1 (1b)|  L2 (9b)  |  L3 (9b)  | Offset(12b)|
       ////   |  0..1  |  0..511   |  0..511   |  0..4095   |
       ////   +--------+-----------+-----------+------------+
       //// - l1_idx (bit 30): selects the lower 1 GB [0x00000000..0x3FFFFFFF] (slot 0) or upper 1 GB [0x40000000..0x7FFFFFFF] (slot 1).
       //// - l2_idx (bits [29:21]): selects one of 512 2 MB regions.
       //// - l3_idx (bits [20:12]): selects one of 512 4 KB pages.
 142 |     /* With T0SZ=33 (31-bit VA = 2GB), translation starts at Level 1:
 143 |      *   L1 index: bits [30:30] (0..1)
 144 |      *   L2 index: bits [29:21] (0..511)
 145 |      *   L3 index: bits [20:12] (0..511)
 146 |      */
 147 |     uint32_t l1_idx = (uint32_t)((vaddr >> 30) & 0x1FFU);
 148 |     uint32_t l2_idx = (uint32_t)((vaddr >> 21) & 0x1FFU);
 149 |     uint32_t l3_idx = (uint32_t)((vaddr >> 12) & 0x1FFU);
 150 | 
 151 |     spin_lock_saved_state_t irqstate;
 152 |     spin_lock_irqsave(&aspace->lock, &irqstate);
 153 | 
       //// Walk L1 -> L2 -> L3 under aspace->lock, allocating intermediate 4 KB table
       //// pages (VM_PAGE_STATE_MMU) on demand:
       //// - Remember pmm_alloc_page() zeroes every newly allocated page via Physmap, so all 512 entries of a newly linked L2 or L3 table start cleanly invalid (PTE_VALID == 0) before the parent table descriptor (PTE_VALID | PTE_TABLE) is written!
       //// - Notice how paddr_to_physmap(l2_pa) and paddr_to_physmap(l3_pa) turn multi-level page-table traversal into trivial C pointer indexing.
 154 |     pte_t *l1 = aspace->l1_virt;
 155 |     if (!(l1[l1_idx] & PTE_VALID)) {
 156 |         paddr_t l2_pa = 0;
 157 |         if (!pmm_alloc_page(VM_PAGE_STATE_MMU, &l2_pa)) {
 158 |             spin_lock_irqrestore(&aspace->lock, irqstate);
 159 |             return -1;
 160 |         }
 161 |         l1[l1_idx] = (l2_pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
 162 |     }
 163 | 
 164 |     paddr_t l2_pa = l1[l1_idx] & PTE_ADDR_MASK;
 165 |     pte_t *l2 = (pte_t *)paddr_to_physmap(l2_pa);
 166 | 
 167 |     if (!(l2[l2_idx] & PTE_VALID)) {
 168 |         paddr_t l3_pa = 0;
 169 |         if (!pmm_alloc_page(VM_PAGE_STATE_MMU, &l3_pa)) {
 170 |             spin_lock_irqrestore(&aspace->lock, irqstate);
 171 |             return -1;
 172 |         }
 173 |         l2[l2_idx] = (l3_pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
 174 |     }
 175 | 
 176 |     paddr_t l3_pa = l2[l2_idx] & PTE_ADDR_MASK;
 177 |     pte_t *l3 = (pte_t *)paddr_to_physmap(l3_pa);
 178 | 
       //// Install the Level-3 leaf descriptor, increment aspace->mapped_pages if
       //// previously unmapped, drop aspace->lock, flush stale TLB entries across all
       //// CPUs via tlb_flush_page(asid, vaddr), and synchronize I/D caches if the page
       //// is executable.
 179 |     if (!(l3[l3_idx] & PTE_VALID)) {
 180 |         aspace->mapped_pages++;
 181 |     }
 182 |     l3[l3_idx] = (paddr & PTE_ADDR_MASK) | flags_to_pte(flags);
 183 |     uint16_t asid = aspace->asid;
 184 | 
 185 |     spin_lock_irqrestore(&aspace->lock, irqstate);
 186 | 
 187 |     tlb_flush_page(asid, vaddr);
 188 |     if (flags & MMU_FLAG_PERM_EXECUTE) {
 189 |         sync_icache_page(paddr);
 190 |     }
 191 |     return 0;
 192 | }
 193 | 
       //// vmm_unmap_page(): Tears down a single 4 KB mapping (invoked by ab_vmar_unmap /
       //// sys_vmar_unmap, e.g., when userboot unmaps bootfs.tar at 0x10000000 after
       //// spawning servers!). Clears l3[l3_idx] = 0 under aspace->lock, decrements
       //// mapped_pages, and broadcasts an Inner-Shareable TLB shootdown across all 4
       //// cores.
 194 | int vmm_unmap_page(vm_aspace_t *aspace, vaddr_t vaddr) {
 195 |     if (!aspace || vaddr < USER_ASPACE_BASE || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE)) {
 196 |         return -1;
 197 |     }
 198 |     vaddr &= ~(PAGE_SIZE - 1);
 199 |     uint32_t l1_idx = (uint32_t)((vaddr >> 30) & 0x1FFU);
 200 |     uint32_t l2_idx = (uint32_t)((vaddr >> 21) & 0x1FFU);
 201 |     uint32_t l3_idx = (uint32_t)((vaddr >> 12) & 0x1FFU);
 202 | 
 203 |     spin_lock_saved_state_t irqstate;
 204 |     spin_lock_irqsave(&aspace->lock, &irqstate);
 205 | 
 206 |     pte_t *l1 = aspace->l1_virt;
 207 |     if (!(l1[l1_idx] & PTE_VALID)) {
 208 |         spin_lock_irqrestore(&aspace->lock, irqstate);
 209 |         return -1;
 210 |     }
 211 |     pte_t *l2 = (pte_t *)paddr_to_physmap(l1[l1_idx] & PTE_ADDR_MASK);
 212 |     if (!(l2[l2_idx] & PTE_VALID)) {
 213 |         spin_lock_irqrestore(&aspace->lock, irqstate);
 214 |         return -1;
 215 |     }
 216 |     pte_t *l3 = (pte_t *)paddr_to_physmap(l2[l2_idx] & PTE_ADDR_MASK);
 217 |     if (!(l3[l3_idx] & PTE_VALID)) {
 218 |         spin_lock_irqrestore(&aspace->lock, irqstate);
 219 |         return -1;
 220 |     }
 221 | 
 222 |     l3[l3_idx] = 0;
 223 |     if (aspace->mapped_pages > 0) aspace->mapped_pages--;
 224 |     uint16_t asid = aspace->asid;
 225 | 
 226 |     spin_lock_irqrestore(&aspace->lock, irqstate);
 227 | 
 228 |     tlb_flush_page(asid, vaddr);
 229 |     return 0;
 230 | }
 231 | 
       //// vmm_resolve_vaddr(): Pure software page-table walker translating a user
       //// virtual address into its backing physical address + raw L3 PTE under
       //// aspace->lock.
       //// Powers two critical subsystems:
       //// 1. Safe EL0<->EL1 memory copy (copy_from_user / copy_to_user in syscalls.c),
       //// which verifies PTE_AP_USER (and !PTE_AP_RO on writes) before accessing user
       //// buffers safely via paddr_to_physmap(*out_paddr).
       //// 2. Process memory introspection (sys_process_get_maps / shell 'vmaps'
       //// command).
 232 | int vmm_resolve_vaddr(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t *out_paddr, pte_t *out_pte) {
 233 |     if (!aspace || vaddr < USER_ASPACE_BASE || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE)) {
 234 |         return -1;
 235 |     }
 236 |     uint32_t l1_idx = (uint32_t)((vaddr >> 30) & 0x1FFU);
 237 |     uint32_t l2_idx = (uint32_t)((vaddr >> 21) & 0x1FFU);
 238 |     uint32_t l3_idx = (uint32_t)((vaddr >> 12) & 0x1FFU);
 239 | 
 240 |     spin_lock_saved_state_t irqstate;
 241 |     spin_lock_irqsave(&aspace->lock, &irqstate);
 242 | 
 243 |     pte_t *l1 = aspace->l1_virt;
 244 |     if (!(l1[l1_idx] & PTE_VALID)) {
 245 |         spin_lock_irqrestore(&aspace->lock, irqstate);
 246 |         return -1;
 247 |     }
 248 |     pte_t *l2 = (pte_t *)paddr_to_physmap(l1[l1_idx] & PTE_ADDR_MASK);
 249 |     if (!(l2[l2_idx] & PTE_VALID)) {
 250 |         spin_lock_irqrestore(&aspace->lock, irqstate);
 251 |         return -1;
 252 |     }
 253 |     pte_t *l3 = (pte_t *)paddr_to_physmap(l2[l2_idx] & PTE_ADDR_MASK);
 254 |     pte_t pte = l3[l3_idx];
 255 |     spin_lock_irqrestore(&aspace->lock, irqstate);
 256 | 
 257 |     if (!(pte & PTE_VALID)) return -1;
 258 | 
 259 |     if (out_paddr) *out_paddr = (pte & PTE_ADDR_MASK) | (vaddr & (PAGE_SIZE - 1));
 260 |     if (out_pte)   *out_pte   = pte;
 261 |     return 0;
 262 | }
 263 | 
       //// vmm_switch_aspace(vm_aspace_t *aspace): Ultra-fast hardware address-space
       //// switch called by sched_reschedule_locked() and user_thread_trampoline():
       //// - Packs the 16-bit ASID into TTBR0_EL1[63:48] and the L1 physical base into TTBR0_EL1[47:12] (or loads empty_ttbr0_l1 with ASID 0 for kernel/idle threads).
       //// - Executes 'msr ttbr0_el1, %0' + 'isb' — switching the entire 2 GB user address space in two instructions with ZERO TLB flushes because all user PTEs carry PTE_NG (ASID-tagged)!
 264 | void vmm_switch_aspace(vm_aspace_t *aspace) {
 265 |     uint64_t ttbr0_val;
 266 |     if (aspace) {
 267 |         ttbr0_val = ((uint64_t)aspace->asid << 48) | (aspace->ttbr0_phys & PTE_ADDR_MASK);
 268 |     } else {
 269 |         ttbr0_val = physmap_to_paddr(empty_ttbr0_l1) & PTE_ADDR_MASK;
 270 |     }
 271 |     __asm__ volatile(
 272 |         "msr ttbr0_el1, %0\n"
 273 |         "isb\n"
 274 |         :: "r"(ttbr0_val) : "memory");
 275 | }
 276 | 
       //// vmm_destroy_aspace(vm_aspace_t *aspace): Reclaims all MMU resources when a
       //// user process exits and its last reference drops (process_destroy in object.c):
       //// - Iterates l1[0..1] (the only two L1 entries reachable in a 2 GB T0SZ=33 space!) and l2[0..511], freeing every valid L3 table page, L2 table page, and finally the L1 root page (ttbr0_phys) back to PMM.
       //// - Why doesn't vmm_destroy_aspace() call pmm_free_page() on the leaf entries INSIDE l3[0..511]? Because leaf data/code frames belong to reference-counted VMO objects (ab_vmo_dispatcher_t) — for example, shared read-only pages of bootfs.tar or shared memory VMOs! Those physical data frames are freed cleanly by vmo_destroy() when the VMO's own ref_count reaches zero (and private user stack pages are freed by process_destroy).
       //// - Finally recycles aspace->asid via ab_free_asid() (which flushes all TLB entries for that ASID across all 4 cores) and frees the vm_aspace_t struct via kfree().
 277 | void vmm_destroy_aspace(vm_aspace_t *aspace) {
 278 |     if (!aspace) return;
 279 |     pte_t *l1 = aspace->l1_virt;
 280 |     for (uint32_t i = 0; i < 2; i++) {
 281 |         if (l1[i] & PTE_VALID) {
 282 |             paddr_t l2_pa = l1[i] & PTE_ADDR_MASK;
 283 |             pte_t *l2 = (pte_t *)paddr_to_physmap(l2_pa);
 284 |             for (uint32_t j = 0; j < 512; j++) {
 285 |                 if (l2[j] & PTE_VALID) {
 286 |                     pmm_free_page(l2[j] & PTE_ADDR_MASK);
 287 |                 }
 288 |             }
 289 |             pmm_free_page(l2_pa);
 290 |         }
 291 |     }
 292 |     pmm_free_page(aspace->ttbr0_phys);
 293 |     ab_free_asid(aspace->asid);
 294 |     kfree(aspace);
 295 | }
```
### 9.4.3 Technical Notes

1. **Why Hardware ASIDs + `PTE_NG` Eliminate Context-Switch TLB Flushes**:
   On early MMU architectures without ASIDs (such as 32-bit x86 `CR3` prior to PCID), every switch between Process A and Process B flushed the entire user TLB, causing dozens of cold page-table walks on every IPC round-trip! In a microkernel where every keystroke or file read hops across `shell -> console-srv` or `shell -> fs-srv` via Channels and Ports, flushing the TLB on every switch would cripple IPC throughput. By tagging every user PTE with `PTE_NG` (line 114) and packing `aspace->asid` into `TTBR0_EL1[63:48]` inside `vmm_switch_aspace()` (line 267), translations for `userboot` (ASID 1), `console-srv` (ASID 2), `fs-srv` (ASID 3), and `shell-0..3` (ASIDs 4..7) coexist simultaneously inside each Cortex-A53 core's hardware TLB!

2. **Ownership Separation Between `vmm_destroy_aspace()` and `vmo_destroy()`**:
   New readers often pause at lines 280–292 of `mmu.c` and ask: *"Why does `vmm_destroy_aspace()` free `l2[j]` (the L3 page-table page) and `l2_pa` (the L2 page-table page) and `ttbr0_phys` (the L1 page-table page), without looping over `l3[0..511]` to free the mapped physical pages?"*
   Because in Zircon and ABOS's object-oriented VM architecture, **`vm_aspace_t` owns ONLY `VM_PAGE_STATE_MMU` page-table pages**, whereas **data/code pages (`VM_PAGE_STATE_OBJECT`) are owned by `ab_vmo_dispatcher_t` objects**! Think about what would happen if `vmm_destroy_aspace()` blindly called `pmm_free_page()` on every valid entry in `l3[]`:
   - When `userboot` maps `bootfs.tar` read-only and later exits or unmaps it, freeing `l3[]` targets directly would corrupt shared VMO pages still referenced by another handle!
   - When `console-srv` maps PL011 UART MMIO (`PA 0x09000000`), freeing `l3[]` targets would pass an MMIO address to `pmm_free_page()`!
   Instead, `process_destroy()` (`object.c`) unmaps VMARs (dropping each VMO's `ref_count` so `vmo_destroy()` frees private `VM_PAGE_STATE_OBJECT` pages cleanly when `ref_count == 0`), frees the process's private user-stack pages, and then calls `vmm_destroy_aspace()` to reclaim 100% of the `VM_PAGE_STATE_MMU` page-table tree.

3. **Why `tlbi vae1is` (Inner Shareable) Is Mandatory Even for Single-Threaded User Processes**:
   Even if an EL0 process has only a single thread (`shell-0`), remember from Chapter 8 (`thread.c`) that unpinned threads (`pinned_cpu == -1`) **migrate freely across CPUs 0, 1, 2, and 3**! If `shell-0` ran on CPU 2 ten milliseconds ago (caching TLB entries for `0x10000000` on CPU 2), migrated to CPU 0, and called `sys_vmar_unmap(0x10000000)` on CPU 0, using a local-only `tlbi vae1` would leave a stale TLB entry lingering inside CPU 2! Broadcasting `tlbi vae1is` (`is` = Inner Shareable) invalidates matching TLB entries across all four Cortex-A53 cores in hardware without requiring software IPI interrupts.

---

## 9.5 Summary

Chapter 9 completed our tour of the microkernel's hardware, scheduling, and memory foundations:
- **`kernel/mm/pmm.c`** tracks all 32,768 physical 4 KB frames of DRAM in `page_array[]` across 7 explicit `vm_page_state_t` categories, serving O(1) page allocations scrubbed outside `pmm_lock`, contiguous multi-page runs for stacks/heap/DMA, and post-boot initrd page reclamation (`pmm_free_wired_range`).
- **`kernel/mm/heap.c`** provides 16-byte-aligned `kmalloc()`, `kzalloc()`, and `kfree()` over a 512 KB `Physmap` pool guarded by `0xAB054EA9` magic headers and single-pass physical-adjacency coalescing.
- **`kernel/mm/mmu.c`** isolates every EL0 process inside its own ASID-tagged 3-level `TTBR0_EL1` address space (`vm_aspace_t`), enforcing a NULL guard page (`0x1000` base), hardware `PXN`/`UXN` permissions, SMP-coherent `tlbi *is` TLB shootdowns, and Harvard `dc cvau` + `ic ialluis` instruction-cache synchronization.

With booting (Chapter 6), system bring-up (Chapter 7), trap/IRQ/SMP scheduling (Chapter 8), and 3-tier memory management (Chapter 9) firmly in place, we are ready for **Chapter 10** and **Chapter 11**, which build ABOS's crown jewel atop these primitives: the Zircon-style **Kernel Object & Capability Dispatcher layer (`object.c`)** and the **34-syscall boundary (`syscalls.c`)**.
