# 11 System Call Layer and User-Pointer Validation (syscalls.c)

In a capability-based microkernel, the system call layer is the single architectural choke point separating untrusted `EL0` user-space processes—including standalone device drivers, file servers, shells, and transient applications—from privileged `EL1` kernel state. Where Chapter 8 of Dr. Zhao Jiong's *A Heavily Commented Linux Kernel Source Code* examines `kernel/sys.c` alongside segmented x86 `fs`-register user-memory helpers (`get_fs_byte` / `put_fs_byte` / `verify_area`), this chapter dissects the 1,219 lines of [`lesson_20/kernel/core/syscalls.c`](../../lesson_20/kernel/core/syscalls.c), which implements all 34 ABOS system calls, software-walked `EL0 <-> EL1` memory copy primitives, fault-isolated process teardown, and central trap dispatch.

---

## 11.1 Main Functions and Boundary Security

Every transition across the `EL0 -> EL1` privilege boundary in `syscalls.c` is governed by five interlocking architectural invariants: (1) a fixed register-and-trap ABI via `SVC #0`, (2) copy-once user-space argument descriptor structures for calls exceeding four parameters, (3) strict prohibition of raw `EL0` pointer dereferences inside `EL1`, (4) heap allocation of large diagnostic buffers to safeguard the 8 KB kernel stack, and (5) deterministic `TTBR0_EL1` evacuation and lock-split capability cleanup when a process exits or crashes.

### The `SVC #0` Calling Convention (`x8` + `x0..x3`)

On AArch64, user-space libraries invoke the microkernel through `__ab_syscall4()` in `user/ulibc/ulibc.c`. The caller places the system call number (`1..34`) in register `x8` and up to four 64-bit arguments in registers `x0` through `x3`, then executes `svc #0`:

```text
  EL0 User Process (ulibc: __ab_syscall4)
  +-------------------------------------------------------------------+
  |  x8 = syscall_num (1..34)                                         |
  |  x0 = arg0,  x1 = arg1,  x2 = arg2,  x3 = arg3                    |
  |  svc #0   ------------------>  Hardware switches to EL1h (SP_EL1) |
  +-------------------------------------------------------------------+
                                   Saves PSTATE -> SPSR_EL1, PC+4 -> ELR_EL1
                                   Masks IRQs (PSTATE.I = 1), sets ESR_EL1.EC = 0x15
                                                |
                                                v
  EL1 Vector Stub (exceptions.S -> arm64_sync_exception in exceptions_c.c)
  +-------------------------------------------------------------------+
  |  1. Pushes 288-byte iframe_t (x0..x30, sp_el0, elr, spsr, mdscr)  |
  |  2. Verifies EC == 0x15 && (flags & ARM64_EXCEPTION_FLAG_LOWER_EL)|
  |  3. arch_enable_ints()        <-- Allows blocking & preemption!   |
  |  4. syscall_dispatch(iframe)  <-- Reads r[8], r[0..3]; sets r[0]  |
  |  5. arch_disable_ints()       <-- Freezes iframe_t for restore    |
  |  6. Pops iframe_t & executes ERET back to EL0 with status in x0   |
  +-------------------------------------------------------------------+
```

Enabling interrupts (`arch_enable_ints()`) immediately before calling `syscall_dispatch(iframe)` is essential for microkernel responsiveness: blocking calls such as `SYS_NANOSLEEP`, `SYS_OBJECT_WAIT_ONE`, `SYS_PORT_WAIT`, and `SYS_INTERRUPT_WAIT` put the calling thread to sleep on LK wait queues while the 100 Hz generic timer and device IRQs continue scheduling other CPUs and threads. Because `SP_EL0`, `ELR_EL1`, and `SPSR_EL1` were saved inside the per-thread `iframe_t` on the calling thread's private `EL1` stack, any number of context switches can occur mid-syscall without corrupting user return state.

The complete ABI contract of all 34 ABOS system calls dispatched by `syscalls.c` is summarized below:

| ID | Symbolic Constant | Handler in `syscalls.c` | `x0..x3` Register / Descriptor Layout | Required Capability Rights | Zircon Equivalent |
|---:|---|---|---|---|---|
| 1 | `SYS_HANDLE_CLOSE` | `sys_handle_close` | `x0`: `handle` | Valid handle in caller table | `zx_handle_close` |
| 2 | `SYS_HANDLE_DUPLICATE` | `sys_handle_duplicate` | `x0`: `handle`, `x1`: `rights`, `x2`: `*user_out` | `AB_RIGHT_DUPLICATE` | `zx_handle_duplicate` |
| 3 | `SYS_OBJECT_GET_INFO` | `sys_object_get_info` | `x0`: `handle`, `x1`: `*user_info` | `AB_RIGHT_INSPECT` | `zx_object_get_info` |
| 4 | `SYS_DEBUGLOG_WRITE` | `sys_debuglog_write` | `x0`: `log_h`, `x1`: `flags`, `x2`: `buf`, `x3`: `len` | `LOG` + `AB_RIGHT_WRITE` | `zx_debuglog_write` |
| 5 | `SYS_PROCESS_EXIT` | `ab_process_exit_current` | `x0`: `exit_code` (noreturn) | Implicit (self process) | `zx_process_exit` |
| 6 | `SYS_NANOSLEEP` | `thread_sleep_ns` | `x0`: `delay_ns` | None (pure time delay) | `zx_nanosleep` |
| 7 | `SYS_EVENT_CREATE` | `sys_event_create` | `x0`: `options`, `x1`: `*user_out` | None (allocates Event) | `zx_event_create` |
| 8 | `SYS_EVENTPAIR_CREATE` | `sys_eventpair_create` | `x0`: `options`, `x1`: `*out0`, `x2`: `*out1` | None (allocates EventPair) | `zx_eventpair_create` |
| 9 | `SYS_OBJECT_SIGNAL` | `sys_object_signal` | `x0`: `handle`, `x1`: `clear_mask`, `x2`: `set_mask` | `AB_RIGHT_SIGNAL` | `zx_object_signal` |
| 10 | `SYS_OBJECT_SIGNAL_PEER` | `sys_object_signal_peer` | `x0`: `handle`, `x1`: `clear_mask`, `x2`: `set_mask` | `EVENTPAIR` + `SIGNAL_PEER` | `zx_object_signal_peer` |
| 11 | `SYS_OBJECT_WAIT_ONE` | `sys_object_wait_one` | `x0`: `h`, `x1`: `signals`, `x2`: `deadline`, `x3`: `*obs` | `AB_RIGHT_WAIT` | `zx_object_wait_one` |
| 12 | `SYS_PORT_CREATE` | `sys_port_create` | `x0`: `options`, `x1`: `*user_out` | None (allocates Port) | `zx_port_create` |
| 13 | `SYS_PORT_QUEUE` | `sys_port_queue` | `x0`: `port_h`, `x1`: `*user_pkt` | `PORT` + `AB_RIGHT_WRITE` | `zx_port_queue` |
| 14 | `SYS_PORT_WAIT` | `sys_port_wait` | `x0`: `port_h`, `x1`: `deadline`, `x2`: `*out_pkt` | `PORT` + `AB_RIGHT_READ` | `zx_port_wait` |
| 15 | `SYS_OBJECT_WAIT_ASYNC` | `sys_object_wait_async` | `x0`: `h`, `x1`: `port_h`, `x2`: `key`, `x3`: `signals` | `WAIT` on `h` + `WRITE` on `port` | `zx_object_wait_async` |
| 16 | `SYS_CHANNEL_CREATE` | `sys_channel_create` | `x0`: `options`, `x1`: `*out0`, `x2`: `*out1` | None (allocates Channel pair) | `zx_channel_create` |
| 17 | `SYS_CHANNEL_WRITE` | `sys_channel_write` | `x0`: `ch_h`, `x1`: `options`, `x2`: `*write_args` | `CHANNEL` + `WRITE` (+ `TRANSFER`) | `zx_channel_write` |
| 18 | `SYS_CHANNEL_READ` | `sys_channel_read` | `x0`: `ch_h`, `x1`: `options`, `x2`: `*read_args` | `CHANNEL` + `AB_RIGHT_READ` | `zx_channel_read` |
| 19 | `SYS_VMO_CREATE` | `sys_vmo_create` | `x0`: `size`, `x1`: `options`, `x2`: `*user_out` | None (allocates RAM VMO) | `zx_vmo_create` |
| 20 | `SYS_VMO_READ` | `sys_vmo_read` | `x0`: `vmo_h`, `x1`: `buf`, `x2`: `offset`, `x3`: `len` | `VMO` + `AB_RIGHT_READ` | `zx_vmo_read` |
| 21 | `SYS_VMO_WRITE` | `sys_vmo_write` | `x0`: `vmo_h`, `x1`: `buf`, `x2`: `offset`, `x3`: `len` | `VMO` + `AB_RIGHT_WRITE` | `zx_vmo_write` |
| 22 | `SYS_VMAR_MAP` | `sys_vmar_map` | `x0`: `vmar_h`, `x1`: `vmar_off`, `x2`: `*map_args` | `MAP` on VMAR + `MAP`/perms on VMO | `zx_vmar_map` |
| 23 | `SYS_PROCESS_CREATE` | `sys_process_create` | `x0`: `job_h`, `x1`: `*create_args` | `JOB` + `MANAGE_PROCESS` | `zx_process_create` |
| 24 | `SYS_THREAD_CREATE` | `sys_thread_create` | `x0`: `proc_h`, `x1`: `name`, `x2`: `len`, `x3`: `*out` | `PROCESS` + `MANAGE_THREAD` | `zx_thread_create` |
| 25 | `SYS_PROCESS_START` | `sys_process_start` | `x0`: `proc_h`, `x1`: `*start_args` | `WRITE` on Process & Thread | `zx_process_start` |
| 26 | `SYS_VMO_CREATE_PHYSICAL` | `sys_vmo_create_physical` | `x0`: `res_h`, `x1`: `*phys_args` | `RESOURCE` + `AB_RIGHT_WRITE` | `zx_vmo_create_physical` |
| 27 | `SYS_VMO_CREATE_CONTIG` | `sys_vmo_create_contiguous` | `x0`: `res_h`, `x1`: `*contig_args` | `RESOURCE` + `AB_RIGHT_WRITE` | `zx_vmo_create_contiguous` |
| 28 | `SYS_INTERRUPT_CREATE` | `sys_interrupt_create` | `x0`: `res_h`, `x1`: `irq_num`, `x2`: `*user_out` | `RESOURCE` + `AB_RIGHT_READ` | `zx_interrupt_create` |
| 29 | `SYS_INTERRUPT_WAIT` | `sys_interrupt_wait` | `x0`: `intr_h` | `INTERRUPT` + `AB_RIGHT_WAIT` | `zx_interrupt_wait` |
| 30 | `SYS_INTERRUPT_ACK` | `sys_interrupt_ack` | `x0`: `intr_h` | `INTERRUPT` + `AB_RIGHT_WRITE` | `zx_interrupt_ack` |
| 31 | `SYS_OBJECT_SET_NAME` | `sys_object_set_name` | `x0`: `handle`, `x1`: `name`, `x2`: `len` | Any valid handle | `zx_object_set_property` |
| 32 | `SYS_JOB_GET_PS_INFO` | `sys_job_get_ps_info` | `x0`: `job_h`, `x1`: `entries`, `x2`: `max`, `x3`: `*act` | `JOB` + `AB_RIGHT_INSPECT` | `zx_object_get_info` (tasks) |
| 33 | `SYS_PROCESS_GET_MAPS` | `sys_process_get_maps` | `x0`: `job_h`, `x1`: `*maps_args` | `JOB` + `AB_RIGHT_INSPECT` | `zx_object_get_info` (maps) |
| 34 | `SYS_VMAR_UNMAP` | `sys_vmar_unmap` | `x0`: `vmar_h`, `x1`: `addr`, `x2`: `len` | `VMAR` + `AB_RIGHT_MAP` | `zx_vmar_unmap` |

### Packing >4 Arguments into Descriptor Structs

Because `__ab_syscall4` standardizes on four argument registers (`x0..x3`), system calls requiring five to eight logical inputs/outputs—most notably `channel_write` (6 parameters), `channel_read` (8 parameters), `vmar_map` (7 parameters), and `process_start` (6 parameters), as well as `process_create`, `vmo_create_physical`, `vmo_create_contiguous`, and `process_get_maps`—pack their auxiliary parameters into a typed C descriptor struct in `include/object.h` (`ab_channel_write_args_t`, `ab_channel_read_args_t`, `ab_vmar_map_args_t`, `ab_process_start_args_t`, etc.) on the caller's `EL0` stack and pass its address in `x1` or `x2`.

Every struct-taking handler obeys a strict **two-stage copy-before-check** protocol to eliminate Time-of-Check-to-Time-of-Use (TOCTOU) race conditions on SMP:
1. First, `copy_from_user(&kargs, user_args, sizeof(kargs))` copies the entire descriptor struct onto the `EL1` stack *once*.
2. Second, all bounds, flag, handle, and nested buffer validations operate exclusively on the private kernel copy `kargs`—never re-reading fields from `user_args`—so a concurrent `EL0` thread sharing the caller's `VmAspace` cannot mutate pointer or length fields mid-syscall.

### Why `EL1` Must Never Dereference Raw `EL0` Pointers

Even though `TTBR0_EL1` remains loaded with the calling process's page table during a system call, `EL1` code in `syscalls.c` **never** reads or writes `*user_ptr` directly. Doing so on AArch64 would open four severe security and stability vulnerabilities:

1. **Confused-Deputy Kernel Memory Read/Write (`TTBR1_EL1` Forgery)**: In `EL1`, virtual addresses in `0xFFFFFFFF80000000..0xFFFFFFFFFFFFFFFF` resolve through `TTBR1_EL1` to kernel code, data, heap, physmap, and GIC/UART MMIO registers. If `sys_vmo_read` or `sys_object_get_info` dereferenced `user_buf` directly, a malicious `EL0` process could pass a kernel address and trick the kernel into overwriting its own `thread_t` or handle table! `user_range_valid()` blocks this at the gate by restricting all user ranges to `[0x1000, 0x80000000)`.
2. **Preventing `EL1` Synchronous Data Abort Panics & Supporting Lazy VMARs**: In `exceptions_c.c`, a translation or permission fault taken while executing in `EL0` (`EC = 0x24`) either triggers VMO demand-paging or terminates only the offending user process; by contrast, an unhandled fault taken while executing in `EL1` (`EC = 0x25`) halts the entire system with `[PANIC] Unhandled EL1 Kernel Sync Exception`! Walking `proc->aspace` in software via `resolve_user_page()` allows `EL1` to fault in lazy uncommitted VMAR pages cleanly via `ab_process_handle_page_fault()` and return `AB_ERR_INVALID_ARGS` or `AB_ERR_ACCESS_DENIED` on unmapped or read-only pages without ever risking an `EL1` hardware abort.
3. **Bypassing Read-Only PTEs & Hardware PAN Absence**: Base ARMv8.0-A (prior to ARMv8.1 Privileged Access Never, `PAN`) permits `EL1` reads on any `EL0`-accessible page, and copying through an unchecked physmap alias would bypass `PTE_AP_RO` entirely. `resolve_user_page()` explicitly enforces `pte & PTE_AP_USER` on all accesses and `!(pte & PTE_AP_RO)` whenever `is_write == 1`.
4. **Blocking Device MMIO Hazards**: User-space drivers map physical MMIO frames into `TTBR0_EL1` via `SYS_VMO_CREATE_PHYSICAL`. `resolve_user_page()` verifies `(pte & (7ULL << 2)) == PTE_ATTR_NORMAL` and `0x40000000ULL <= pa < 0x80000000ULL`, ensuring `EL0` cannot pass device register pages into kernel memory copy loops or cause `paddr_to_physmap(pa)` to index outside physical RAM.

### Heap Allocation of Large `ps` and `vmaps` Buffers to Protect the 8 KB Kernel Stack

Each LK kernel thread in ABOS runs on a fixed **8 KB (`THREAD_STACK_SIZE = 2 * PAGE_SIZE = 8,192` bytes)** `EL1` stack. When an `EL0` thread enters a system call, 288 bytes are immediately consumed by the `SVC` `iframe_t`, plus activation records for `arm64_sync_exception`, `syscall_dispatch`, the syscall handler, object/MMU callees, and—because interrupts are enabled during syscalls—potentially another 288-byte IRQ `iframe_t` plus `arm64_irq_exception` and scheduler frames!

Small, fixed-size staging buffers (`char kbuf[257]` in `sys_debuglog_write`, `uint8_t kbytes[256]` + `ab_handle_t khandles[8]` in `sys_channel_write`/`read`, and `uint8_t kchunk[256]` in `sys_vmo_read`/`write`) consume under 300 bytes and are completely safe on the stack. By contrast, consider the two diagnostic introspection calls:
- In `sys_job_get_ps_info`, `sizeof(ab_task_info_entry_t)` is **152 bytes**; at the 64-entry cap, `64 * 152 = 9,728` bytes (~9.5 KB)—exceeding the entire 8 KB kernel stack all by itself!
- In `sys_process_get_maps`, `sizeof(ab_info_maps_t)` is **104 bytes**; at the 32-entry cap, `32 * 104 = 3,328` bytes (~3.25 KB)—consuming nearly half of the 8 KB stack before counting `iframe_t` and nested IRQ frames.

Both handlers therefore allocate their staging buffers dynamically on the kernel heap with `kzalloc()` (which also zeroes all struct padding bytes so stale kernel heap contents never leak across `copy_to_user`) and release them unconditionally with `kfree()`.

### Safe `TTBR0_EL1` Teardown Order During `ab_process_exit_current`

Tearing down a running user process from inside one of its own threads requires strict ordering across reference counts, hardware MMU registers, and spinlocks:

```text
  ab_process_exit_current(exit_code)
    |
    +--> 1. ab_object_add_ref(&proc->base)   [Pin proc across self-handle & job drops]
    |
    +--> 2. cur->aspace = NULL;              [Detach LK thread pointers FIRST]
    |       vmm_switch_aspace(NULL)          [Point TTBR0_EL1 -> empty_ttbr0_l1 + ISB]
    |
    +--> 3. Lock vmar_lock -> vmm_unmap_page [Unmap PTEs & collect vmos_to_rel[]]
    |       Unlock vmar_lock -> release VMOs [Free VMO refs OUTSIDE IRQ spinlock]
    |
    +--> 4. Scan handles[0..63]:             [Snapshot gen under handle_lock, then]
    |       handle_close(proc, h) unlocked   [fire on_zero_handles / PEER_CLOSED!]
    |
    +--> 5. Signal AB_SIGNAL_TASK_TERMINATED [Wake shell / supervisor waiters]
            ab_job_remove_process(proc)      [Unlink from parent Job]
            ab_object_release(&proc->base)   [Drop pin -> process_destroy when 0]
            thread_exit(exit_code)           [Mark LK thread THREAD_DEATH & yield]
```

Switching `TTBR0_EL1` away to `empty_ttbr0_l1` **before** unmapping VMAR pages or releasing `proc->base` guarantees the CPU's hardware table walker can never speculatively walk page-table pages that are concurrently being unmapped or freed back to the PMM.

---

## 11.2 User-Pointer Validation (copy_from_user and copy_to_user)

### 11.2.1 Functional Description

Lines 1–122 of `kernel/core/syscalls.c` establish the low-level memory firewall between `EL0` and `EL1`. Four functions cooperate in a strict pipeline:
- `user_range_valid(uaddr, len)` verifies in constant time without locks that a non-empty byte range lies strictly within `[USER_ASPACE_BASE, USER_ASPACE_BASE + USER_ASPACE_SIZE)` (`[0x1000, 0x80000000)`) with no unsigned 64-bit wraparound.
- `resolve_user_page(proc, page_va, is_write, out_pa)` walks `proc->aspace` via `vmm_resolve_vaddr`. On a missing PTE, it invokes `ab_process_handle_page_fault(proc, page_va, is_write, 0)` once to materialize any uncommitted lazy VMAR page and re-queries `vmm_resolve_vaddr`. It then checks `PTE_AP_USER`, write permission (`!PTE_AP_RO` when `is_write == 1`), Normal cacheable RAM memory type (`PTE_ATTR_NORMAL`), and physical RAM bounds (`0x40000000 <= pa < 0x80000000`).
- `copy_from_user(kdst, usrc, len)` and `copy_to_user(udst, ksrc, len)` slice arbitrary byte ranges along 4 KB (`PAGE_SIZE`) boundaries, resolve each page to its physical address, convert that physical address into a kernel virtual address via `paddr_to_physmap(pa)`, and copy `chunk` bytes safely inside `TTBR1_EL1`.

### 11.2.2 Heavily Commented Source Code

#### Program 11-1: `lesson_20/kernel/core/syscalls.c` (Lines 1–122)

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/syscalls.c" start="1" end="122" total="1219" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: kernel/core/syscalls.c (Lines 1-1219)
       //// ==========================================================================
       //// This file implements the entire EL0 <-> EL1 system call boundary of ABOS,
       //// playing the same architectural role as kernel/sys.c plus the user-memory
       //// accessors (get_fs_byte / put_fs_byte / verify_area) in Linux 0.11.
       ////
       //// Unlike a monolithic POSIX kernel where system calls accept integer file
       //// descriptors and ambient string paths, every privileged operation in ABOS
       //// is mediated by an unforgeable 32-bit capability handle (ab_handle_t) and
       //// validated against a fine-grained bitmask of rights (ab_rights_t).
   1 | /* =========================================================================
   2 |  * syscalls.c - Capability & Signal System Call Implementation
   3 |  * ========================================================================= */
       //// Header dependencies for the system call layer:
       ////   - syscalls.h : Syscall IDs (1..34), syscall_dispatch(), copy helpers,
       ////                  and transitive access to object.h and exceptions.h.
       ////   - thread.h   : LK thread_t, get_current_thread(), thread_sleep_ns(),
       ////                  and thread_exit() for blocking and terminating calls.
       ////   - heap.h     : kmalloc()/kzalloc()/kfree() used by introspection calls
       ////                  (ps and vmaps) to keep large arrays off the 8 KB stack.
       ////   - mmu.h      : USER_ASPACE_BASE/SIZE, PTE permission/attribute bits,
       ////                  vmm_resolve_vaddr(), vmm_switch_aspace(), and physmap.
       ////   - debuglog.h : Kernel console logging via dprintf().
   4 | #include "syscalls.h"
   5 | #include "thread.h"
   6 | #include "heap.h"
   7 | #include "mmu.h"
   8 | #include "debuglog.h"
   9 | 
  10 | /* =========================================================================
  11 |  * Safe EL0 <-> EL1 User Pointer Validation & Copy Helpers
  12 |  * ========================================================================= */
       //// user_range_valid() performs fast, lock-free arithmetic validation on a
       //// candidate EL0 virtual buffer [uaddr, uaddr + len) before any page-table
       //// locks are acquired.
       ////
       //// Zero-length buffers (len == 0) trivially succeed because neither
       //// copy_from_user() nor copy_to_user() will touch memory when len is 0.
  13 | static int user_range_valid(uintptr_t uaddr, size_t len) {
  14 |     if (len == 0) return 1;
       //// Enforce strict confinement within the 2 GB TTBR0_EL1 user address space:
       ////   USER_ASPACE_BASE = 0x0000000000001000 (4 KB null-pointer guard page)
       ////   limit            = 0x0000000080000000 (2 GB upper boundary, T0SZ=33)
       ////
       //// CRITICAL OVERFLOW IDIOM: Writing (uaddr + len > limit) would be a
       //// classic kernel vulnerability! A malicious EL0 caller passing
       //// uaddr = 0x000000007FFFF000 and len = 0xFFFFFFFF80002000 would wrap
       //// 64-bit unsigned addition around zero and pass a naive bound check.
       //// Checking (uaddr >= limit) first and then subtracting (len > limit - uaddr)
       //// is mathematically immune to 64-bit integer wraparound.
  15 |     uintptr_t limit = USER_ASPACE_BASE + USER_ASPACE_SIZE;
  16 |     if (uaddr < USER_ASPACE_BASE || uaddr >= limit) return 0;
  17 |     if (len > limit - uaddr) return 0;
  18 |     return 1;
  19 | }
  20 | 
       //// resolve_user_page() translates a page-aligned user virtual address
       //// (page_va) into a validated physical RAM address (*out_pa), verifying
       //// both VMAR lazy demand-paging rules and hardware Stage-1 PTE permissions.
       ////   - proc     : Calling user process owning the target VmAspace.
       ////   - page_va  : 4 KB-aligned virtual address in [0x1000, 0x80000000).
       ////   - is_write : 0 when kernel reads from user (copy_from_user);
       ////                1 when kernel writes to user (copy_to_user).
  21 | static ab_status_t resolve_user_page(ab_process_t *proc, vaddr_t page_va,
  22 |                                      int is_write, paddr_t *out_pa) {
  23 |     if (!proc || !proc->aspace) return AB_ERR_BAD_STATE;
  24 | 
  25 |     paddr_t pa = 0;
  26 |     pte_t pte  = 0;
       //// Walk the process's 3-level Stage-1 page tables (L1 -> L2 -> L3) via
       //// vmm_resolve_vaddr(). If the L3 PTE is not yet present, the address might
       //// still belong to a valid VMAR mapping whose backing VMO pages have not
       //// yet been committed (lazy demand paging)!
       ////
       //// For example, if an EL0 program calls sys_vmo_create() + sys_vmar_map()
       //// and immediately passes the freshly mapped buffer into sys_channel_read()
       //// or sys_vmo_read() without touching it in EL0 first, no hardware Data
       //// Abort has occurred yet. Calling ab_process_handle_page_fault() allocates
       //// and maps the backing page on demand, after which vmm_resolve_vaddr() is
       //// retried once. Notice is_exec is passed as 0 because syscall data copies
       //// never fetch instructions.
  27 |     if (vmm_resolve_vaddr(proc->aspace, page_va, &pa, &pte) != 0) {
  28 |         /* Page may belong to a lazy VMAR mapping not yet faulted in */
  29 |         if (ab_process_handle_page_fault(proc, page_va, is_write, 0) != 0) {
  30 |             return AB_ERR_INVALID_ARGS;
  31 |         }
  32 |         if (vmm_resolve_vaddr(proc->aspace, page_va, &pa, &pte) != 0) {
  33 |             return AB_ERR_INVALID_ARGS;
  34 |         }
  35 |     }
  36 | 
       //// Software privilege check #1: Require PTE_AP_USER (ARMv8 AP[1] == 1).
       //// Even if a PTE exists inside TTBR0_EL1, EL1 must never read or write a
       //// supervisor-only page on behalf of an unprivileged EL0 system call.
  37 |     if (!(pte & PTE_AP_USER)) {
  38 |         return AB_ERR_ACCESS_DENIED;
  39 |     }
       //// Software privilege check #2: When copying kernel data INTO user memory
       //// (is_write == 1), reject read-only pages (PTE_AP_RO, ARMv8 AP[2] == 1).
       //// Without this explicit check, copying through the kernel's writable
       //// higher-half physmap alias below would silently bypass read-only VMAR
       //// permissions (for instance, overwriting a read-only BOOTFS mapping)!
  40 |     if (is_write && (pte & PTE_AP_RO)) {
  41 |         /* May be a writable VMAR mapping where PTE wasn't upgraded, or truly RO */
  42 |         return AB_ERR_ACCESS_DENIED;
  43 |     }
       //// Software privilege check #3: Reject Device-nGnRnE MMIO mappings and any
       //// physical address outside QEMU virt's main RAM window [1 GB, 2 GB):
       ////   - Bits [4:2] of an ARMv8 L3 PTE select the MAIR_EL1 attribute index;
       ////     only PTE_ATTR_NORMAL (index 1, Normal Write-Back Cacheable RAM) is
       ////     permitted as a syscall copy buffer.
       ////   - User-space drivers map PL011 UART or virtio-mmio registers via
       ////     sys_vmo_create_physical(). Blocking those pages here prevents an EL0
       ////     caller from tricking EL1 into triggering MMIO register side effects
       ////     or indexing paddr_to_physmap(pa) outside the 1 GB RAM physmap.
  44 |     /* Reject Device MMIO pages in syscall memory buffers */
  45 |     if ((pte & (7ULL << 2)) != PTE_ATTR_NORMAL ||
  46 |         pa < 0x40000000ULL || pa >= 0x80000000ULL) {
  47 |         return AB_ERR_ACCESS_DENIED;
  48 |     }
  49 | 
  50 |     *out_pa = pa;
  51 |     return AB_OK;
  52 | }
  53 | 
       //// copy_from_user() safely copies 'len' bytes from an untrusted EL0 source
       //// buffer 'usrc' into a trusted EL1 kernel buffer 'kdst'.
       ////
       //// Notice that 'usrc' is NEVER dereferenced directly as a pointer in C!
       //// Instead, each spanned 4 KB page is resolved to its physical address and
       //// read through the kernel's permanent TTBR1_EL1 linear physmap.
  54 | ab_status_t copy_from_user(void *kdst, const void *usrc, size_t len) {
  55 |     if (len == 0) return AB_OK;
  56 |     if (!kdst || !usrc) return AB_ERR_INVALID_ARGS;
  57 | 
  58 |     uintptr_t uaddr = (uintptr_t)usrc;
  59 |     if (!user_range_valid(uaddr, len)) {
  60 |         return AB_ERR_INVALID_ARGS;
  61 |     }
  62 | 
       //// Look up the calling process from get_current_thread()->process_obj and
       //// prepare to iterate across one or more 4 KB page boundaries.
  63 |     ab_process_t *proc = get_current_process();
  64 |     uint8_t *dst = (uint8_t *)kdst;
  65 |     size_t done = 0;
  66 | 
       //// Page-splitting loop: An unaligned user buffer (e.g., 128 bytes starting
       //// at 0x40000FC0) straddles two distinct virtual and potentially
       //// non-contiguous physical pages:
       ////   +-----------------------+-----------------------+
       ////   | Page N (0x40000000)   | Page N+1 (0x40001000) |
       ////   |          [0xFC0..FFF] | [0x000..0x03F]        |
       ////   |           chunk = 64B | chunk = 64B           |
       ////   +-----------------------+-----------------------+
       //// In each iteration:
       ////   - page_va  rounds cur_va down to the 4 KB page boundary.
       ////   - page_off extracts the byte offset [0..4095] within that page.
       ////   - chunk    is min(bytes left in current 4 KB page, len - done).
  67 |     while (done < len) {
  68 |         uintptr_t cur_va = uaddr + done;
  69 |         vaddr_t page_va  = cur_va & ~(PAGE_SIZE - 1);
  70 |         size_t page_off  = (size_t)(cur_va & (PAGE_SIZE - 1));
  71 |         size_t chunk     = PAGE_SIZE - page_off;
  72 |         if (chunk > len - done) chunk = len - done;
  73 | 
       //// Resolve page_va with is_write = 0 (read access). Convert the validated
       //// physical address 'pa' into its kernel virtual alias via paddr_to_physmap()
       //// (0xFFFFFFFF40000000 + pa -> 0xFFFFFFFF80000000..0xFFFFFFFFBFFFFFFF) and
       //// copy 'chunk' bytes into kdst.
  74 |         paddr_t pa = 0;
  75 |         ab_status_t st = resolve_user_page(proc, page_va, 0, &pa);
  76 |         if (st != AB_OK) return st;
  77 | 
  78 |         const uint8_t *kva = (const uint8_t *)paddr_to_physmap(pa) + page_off;
  79 |         for (size_t i = 0; i < chunk; i++) {
  80 |             dst[done + i] = kva[i];
  81 |         }
  82 |         done += chunk;
  83 |     }
  84 |     return AB_OK;
  85 | }
  86 | 
       //// copy_to_user() is the symmetric EL1 -> EL0 export primitive: it copies
       //// 'len' bytes from a kernel buffer 'ksrc' into a validated, writable EL0
       //// destination buffer 'udst'.
  87 | ab_status_t copy_to_user(void *udst, const void *ksrc, size_t len) {
  88 |     if (len == 0) return AB_OK;
  89 |     if (!udst || !ksrc) return AB_ERR_INVALID_ARGS;
  90 | 
  91 |     uintptr_t uaddr = (uintptr_t)udst;
  92 |     if (!user_range_valid(uaddr, len)) {
  93 |         return AB_ERR_INVALID_ARGS;
  94 |     }
  95 | 
  96 |     ab_process_t *proc = get_current_process();
  97 |     const uint8_t *src = (const uint8_t *)ksrc;
  98 |     size_t done = 0;
  99 | 
       //// Split the destination range at 4 KB page boundaries identically to
       //// copy_from_user(), calling resolve_user_page() with is_write = 1 on every
       //// page so read-only mappings are rejected with AB_ERR_ACCESS_DENIED and
       //// uncommitted writable VMAR pages are demand-faulted before writing.
 100 |     while (done < len) {
 101 |         uintptr_t cur_va = uaddr + done;
 102 |         vaddr_t page_va  = cur_va & ~(PAGE_SIZE - 1);
 103 |         size_t page_off  = (size_t)(cur_va & (PAGE_SIZE - 1));
 104 |         size_t chunk     = PAGE_SIZE - page_off;
 105 |         if (chunk > len - done) chunk = len - done;
 106 | 
 107 |         paddr_t pa = 0;
 108 |         ab_status_t st = resolve_user_page(proc, page_va, 1, &pa);
 109 |         if (st != AB_OK) return st;
 110 | 
       //// Write 'chunk' bytes through the kernel's higher-half physmap alias.
       //// Because both TTBR0_EL1 user RAM mappings and TTBR1_EL1 physmap mappings
       //// use Normal Inner-Shareable Write-Back attributes (PTE_ATTR_NORMAL |
       //// PTE_SH_INNER), hardware D-cache coherency guarantees EL0 immediately
       //// observes these stores without explicit cache-clean instructions.
 111 |         uint8_t *kva = (uint8_t *)paddr_to_physmap(pa) + page_off;
 112 |         for (size_t i = 0; i < chunk; i++) {
 113 |             kva[i] = src[done + i];
 114 |         }
 115 |         done += chunk;
 116 |     }
 117 |     return AB_OK;
 118 | }
 119 | 
       //// Section banner marking the transition from low-level EL0/EL1 memory
       //// validation primitives to the 34 capability-checked system call handlers.
 120 | /* =========================================================================
 121 |  * System Call Handlers
 122 |  * ========================================================================= */
```

### 11.2.3 Technical Notes

1. **Null-Pointer Guard Page (`0x0..0xFFF`)**: Because `USER_ASPACE_BASE` starts at `0x1000` (4 KB), virtual page 0 is permanently excluded by `user_range_valid()`. Any `NULL` pointer or small struct-field offset from `NULL` passed by buggy `EL0` code fails immediately with `AB_ERR_INVALID_ARGS` before touching `aspace->lock`.
2. **Why `copy_from_user` and `copy_to_user` Re-Resolve Every 4 KB Page**: Two virtually contiguous pages in `EL0` (`0x10000` and `0x11000`) are backed by independent PMM frames at arbitrary physical addresses—and may even belong to two adjacent VMAR mappings with different read/write permissions! Resolving `page_va` at the start of every 4 KB chunk ensures both non-contiguous physical placement and per-page permission transitions are honored accurately.
3. **Cache Coherency Across `TTBR0_EL1` and `TTBR1_EL1` Aliases**: On ARMv8-A with PIPT (Physically Indexed, Physically Tagged) data caches, accessing physical address `pa` through the kernel's `paddr_to_physmap(pa)` (`PTE_ATTR_NORMAL | PTE_SH_INNER`) hits the exact same cache lines as `EL0`'s `TTBR0_EL1` mapping (`PTE_ATTR_NORMAL | PTE_SH_INNER`). No cache flush is required for data reads or writes (whereas instruction pages mapped executable in `vmm_map_page` explicitly run `dc cvau` + `ic ialluis` in `mmu.c`).

---

## 11.3 Handle, DebugLog, Signal, Port and Channel Syscalls

### 11.3.1 Functional Description

Lines 123–502 implement the first 15 capability-checked system calls, covering five core microkernel abstractions:
1. **Handle Lifecycle & Generic Object Inspection** (`sys_handle_close`, `sys_handle_duplicate`, `sys_object_get_info`): Closing handles, duplicating handles with rights attenuation, and inspecting object KOIDs, types, rights, and live handle counts.
2. **Capability-Gated Debug Console** (`sys_debuglog_write`): Writing bounded diagnostic strings (up to 256 bytes) to the kernel UART log when authorized by an `AB_OBJ_TYPE_LOG` handle with `AB_RIGHT_WRITE`.
3. **Events, EventPairs & Object Signals** (`sys_event_create`, `sys_eventpair_create`, `sys_object_signal`, `sys_object_signal_peer`, `sys_object_wait_one`): Creating standalone or peered synchronization objects, mutating user-allowed signal bits (`AB_SIGNAL_SIGNALED` and `AB_USER_SIGNAL_0..3`), and blocking synchronously with optional nanosecond deadlines.
4. **Asynchronous Event Multiplexing Ports** (`sys_port_create`, `sys_port_queue`, `sys_port_wait`, `sys_object_wait_async`): Creating 32-slot packet queues, posting user packets, waiting for packets, and binding one-shot `StateTracker` observers so a single server thread (`console-srv`, `fs-srv`) can multiplex multiple client channels and hardware interrupts without one thread per client.
5. **Bidirectional IPC Channels & Capability Transfer** (`sys_channel_create`, `sys_channel_write`, `sys_channel_read`): Creating entangled message-pipe endpoints and transferring both 256-byte messages and up to 8 live capability handles across protection domains.

### 11.3.2 Heavily Commented Source Code

#### Program 11-2: `lesson_20/kernel/core/syscalls.c` (Lines 123–502)

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/syscalls.c" start="123" end="502" total="1219" -->
```c
       //// --------------------------------------------------------------------------
       //// SYSCALL 1: sys_handle_close (SYS_HANDLE_CLOSE)
       //// --------------------------------------------------------------------------
       //// Closes capability handle 'handle' in the calling process's handle table.
       //// If this was the last open handle to the underlying kernel object,
       //// handle_close() invokes obj->on_zero_handles() (e.g., asserting
       //// AB_SIGNAL_PEER_CLOSED on the opposite end of a Channel or EventPair)
       //// and drops the handle table's reference on obj.
 123 | static ab_status_t sys_handle_close(ab_handle_t handle) {
 124 |     return handle_close(get_current_process(), handle);
 125 | }
 126 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 2: sys_handle_duplicate (SYS_HANDLE_DUPLICATE)
       //// --------------------------------------------------------------------------
       //// Creates a second handle referring to the same kernel object with equal
       //// or strictly attenuated capability rights (rights == AB_RIGHT_SAME_RIGHTS
       //// or (rights & ~orig_rights) == 0, provided orig_rights has
       //// AB_RIGHT_DUPLICATE). Models Zircon's zx_handle_duplicate().
 127 | static ab_status_t sys_handle_duplicate(ab_handle_t handle, ab_rights_t rights,
 128 |                                         ab_handle_t *user_out) {
 129 |     if (!user_out) return AB_ERR_INVALID_ARGS;
 130 |     ab_process_t *proc = get_current_process();
 131 |     ab_handle_t new_h = AB_HANDLE_INVALID;
 132 |     ab_status_t st = handle_duplicate(proc, handle, rights, &new_h);
 133 |     if (st != AB_OK) return st;
 134 | 
       //// TRANSACTIONAL ROLLBACK PATTERN: handle_duplicate() has already installed
       //// new_h in proc->handles[]. If writing new_h out to user_out fails (e.g.,
       //// EL0 passed a read-only or unmapped pointer), we MUST close new_h before
       //// returning the error; otherwise an unreachable capability slot would leak
       //// inside the process's 64-entry handle table!
 135 |     st = copy_to_user(user_out, &new_h, sizeof(new_h));
 136 |     if (st != AB_OK) {
 137 |         handle_close(proc, new_h);
 138 |     }
 139 |     return st;
 140 | }
 141 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 3: sys_object_get_info (SYS_OBJECT_GET_INFO)
       //// --------------------------------------------------------------------------
       //// Queries metadata (KOID, related peer/parent KOID, type, handle rights,
       //// and live handle count) for any capability possessing AB_RIGHT_INSPECT.
       //// Passing AB_OBJ_TYPE_NONE tells handle_lookup() to accept any object type
       //// while incrementing obj->ref_count on success.
 142 | static ab_status_t sys_object_get_info(ab_handle_t handle,
 143 |                                        ab_handle_basic_info_t *user_info) {
 144 |     if (!user_info) return AB_ERR_INVALID_ARGS;
 145 |     ab_object_t *obj = NULL;
 146 |     ab_rights_t rights = 0;
 147 |     ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
 148 |                                    AB_RIGHT_INSPECT, &obj, &rights);
 149 |     if (st != AB_OK) return st;
 150 | 
       //// Assemble ab_handle_basic_info_t on the kernel stack, reading handle_count
       //// atomically, and immediately release the temporary kernel reference via
       //// ab_object_release(obj) BEFORE calling copy_to_user() (which may fault in
       //// a lazy user page).
 151 |     ab_handle_basic_info_t kinfo;
 152 |     kinfo.koid         = obj->koid;
 153 |     kinfo.related_koid = obj->related_koid;
 154 |     kinfo.type         = (uint32_t)obj->type;
 155 |     kinfo.rights       = rights;
 156 |     kinfo.handle_count = (uint32_t)__atomic_load_n(&obj->handle_count, __ATOMIC_RELAXED);
 157 |     ab_object_release(obj);
 158 | 
 159 |     return copy_to_user(user_info, &kinfo, sizeof(kinfo));
 160 | }
 161 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 4: sys_debuglog_write (SYS_DEBUGLOG_WRITE)
       //// --------------------------------------------------------------------------
       //// Capability-gated kernel console output. User processes cannot write to
       //// the PL011 UART unless they hold a valid AB_OBJ_TYPE_LOG handle with
       //// AB_RIGHT_WRITE (minted by lk_main and delegated selectively by userboot).
 162 | static ab_status_t sys_debuglog_write(ab_handle_t log_handle, uint32_t flags,
 163 |                                       const char *user_buf, size_t len) {
 164 |     (void)flags;
 165 |     if (!user_buf || len > 256) return AB_ERR_INVALID_ARGS;
 166 | 
 167 |     ab_object_t *obj = NULL;
 168 |     ab_status_t st = handle_lookup(get_current_process(), log_handle,
 169 |                                    AB_OBJ_TYPE_LOG, AB_RIGHT_WRITE,
 170 |                                    &obj, NULL);
 171 |     if (st != AB_OK) return st;
 172 | 
       //// Copy at most 256 bytes into a 257-byte stack buffer, force a trailing
       //// NUL byte at kbuf[len] so untrusted non-terminated EL0 strings can never
       //// overrun dprintf('%s'), tag the log line with CPU ID, process name, and
       //// log object KOID, and drop the object reference.
 173 |     char kbuf[257];
 174 |     st = copy_from_user(kbuf, user_buf, len);
 175 |     if (st != AB_OK) {
 176 |         ab_object_release(obj);
 177 |         return st;
 178 |     }
 179 |     kbuf[len] = '\0';
 180 | 
 181 |     ab_process_t *proc = get_current_process();
 182 |     dprintf(INFO, "[debuglog cpu=%u proc='%s' koid=%lu] %s\n",
 183 |             arch_curr_cpu_num(), proc ? proc->name : "unknown", obj->koid, kbuf);
 184 | 
 185 |     ab_object_release(obj);
 186 |     return AB_OK;
 187 | }
 188 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 7: sys_event_create (SYS_EVENT_CREATE)
       //// --------------------------------------------------------------------------
       //// Allocates a standalone EventDispatcher (initial ref_count = 1), mints a
       //// handle with AB_RIGHTS_DEFAULT_EVENT (adds a second ref for the handle
       //// table), and drops the local creation reference via ab_object_release(ev)
       //// so the handle table becomes the sole owner.
 189 | static ab_status_t sys_event_create(uint32_t options, ab_handle_t *user_out) {
 190 |     if (!user_out) return AB_ERR_INVALID_ARGS;
 191 |     ab_process_t *proc = get_current_process();
 192 |     ab_object_t *ev = NULL;
 193 |     ab_status_t st = ab_event_dispatcher_create(options, &ev);
 194 |     if (st != AB_OK) return st;
 195 | 
 196 |     ab_handle_t h = AB_HANDLE_INVALID;
 197 |     st = handle_alloc(proc, ev, AB_RIGHTS_DEFAULT_EVENT, &h);
 198 |     ab_object_release(ev);
 199 |     if (st != AB_OK) return st;
 200 | 
 201 |     st = copy_to_user(user_out, &h, sizeof(h));
 202 |     if (st != AB_OK) {
 203 |         handle_close(proc, h);
 204 |     }
 205 |     return st;
 206 | }
 207 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 8: sys_eventpair_create (SYS_EVENTPAIR_CREATE)
       //// --------------------------------------------------------------------------
       //// Creates two mutually linked EventPairDispatchers (ep0 <-> ep1) whose
       //// related_koid fields point to each other. Closing all handles to one side
       //// automatically asserts AB_SIGNAL_PEER_CLOSED on the surviving peer.
 208 | static ab_status_t sys_eventpair_create(uint32_t options,
 209 |                                         ab_handle_t *user_out0,
 210 |                                         ab_handle_t *user_out1) {
 211 |     if (options != 0 || !user_out0 || !user_out1) return AB_ERR_INVALID_ARGS;
 212 |     ab_process_t *proc = get_current_process();
 213 | 
 214 |     ab_object_t *ep0 = NULL, *ep1 = NULL;
 215 |     ab_status_t st = ab_eventpair_dispatcher_create(&ep0, &ep1);
 216 |     if (st != AB_OK) return st;
 217 | 
       //// Two-handle allocation & copy rollback: If allocating h1 fails, close h0;
       //// always release both creation references (ep0, ep1); and if copying either
       //// h0 or h1 to user space fails, close BOTH handles so neither endpoint
       //// leaks in proc->handles[].
 218 |     ab_handle_t h0 = AB_HANDLE_INVALID, h1 = AB_HANDLE_INVALID;
 219 |     st = handle_alloc(proc, ep0, AB_RIGHTS_DEFAULT_EVENTPAIR, &h0);
 220 |     if (st == AB_OK) {
 221 |         st = handle_alloc(proc, ep1, AB_RIGHTS_DEFAULT_EVENTPAIR, &h1);
 222 |         if (st != AB_OK) {
 223 |             handle_close(proc, h0);
 224 |         }
 225 |     }
 226 |     ab_object_release(ep0);
 227 |     ab_object_release(ep1);
 228 |     if (st != AB_OK) return st;
 229 | 
 230 |     st = copy_to_user(user_out0, &h0, sizeof(h0));
 231 |     if (st == AB_OK) {
 232 |         st = copy_to_user(user_out1, &h1, sizeof(h1));
 233 |     }
 234 |     if (st != AB_OK) {
 235 |         handle_close(proc, h0);
 236 |         handle_close(proc, h1);
 237 |     }
 238 |     return st;
 239 | }
 240 | 
       //// SIGNAL SECURITY MASK: Only AB_SIGNAL_SIGNALED (bit 3) and the four
       //// application-defined signals AB_USER_SIGNAL_0..3 (bits 24..27) may be
       //// cleared or asserted directly by EL0! Kernel-maintained state bits such
       //// as AB_SIGNAL_READABLE, AB_SIGNAL_WRITABLE, and AB_SIGNAL_PEER_CLOSED
       //// are strictly off-limits so user code cannot forge IPC readiness.
 241 | #define USER_ALLOWED_SIGNALS (AB_SIGNAL_SIGNALED | AB_USER_SIGNAL_ALL)
 242 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 9: sys_object_signal (SYS_OBJECT_SIGNAL)
       //// --------------------------------------------------------------------------
       //// Modifies user-controllable signal bits on 'handle' itself, requiring
       //// AB_RIGHT_SIGNAL. Wakes any synchronous threads or asynchronous Port
       //// observers watching the newly asserted signal bits.
 243 | static ab_status_t sys_object_signal(ab_handle_t handle,
 244 |                                      ab_signals_t clear_mask,
 245 |                                      ab_signals_t set_mask) {
 246 |     if ((clear_mask & ~USER_ALLOWED_SIGNALS) || (set_mask & ~USER_ALLOWED_SIGNALS)) {
 247 |         return AB_ERR_INVALID_ARGS;
 248 |     }
 249 |     ab_object_t *obj = NULL;
 250 |     ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
 251 |                                    AB_RIGHT_SIGNAL, &obj, NULL);
 252 |     if (st != AB_OK) return st;
 253 | 
 254 |     state_tracker_update_signals(obj, clear_mask, set_mask);
 255 |     ab_object_release(obj);
 256 |     return AB_OK;
 257 | }
 258 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 10: sys_object_signal_peer (SYS_OBJECT_SIGNAL_PEER)
       //// --------------------------------------------------------------------------
       //// Modifies user-controllable signal bits on the peer endpoint of an
       //// EventPair, requiring AB_OBJ_TYPE_EVENTPAIR and AB_RIGHT_SIGNAL_PEER.
       //// Returns AB_ERR_PEER_CLOSED if the opposite endpoint has already closed.
 259 | static ab_status_t sys_object_signal_peer(ab_handle_t handle,
 260 |                                           ab_signals_t clear_mask,
 261 |                                           ab_signals_t set_mask) {
 262 |     if ((clear_mask & ~USER_ALLOWED_SIGNALS) || (set_mask & ~USER_ALLOWED_SIGNALS)) {
 263 |         return AB_ERR_INVALID_ARGS;
 264 |     }
 265 |     ab_object_t *obj = NULL;
 266 |     ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_EVENTPAIR,
 267 |                                    AB_RIGHT_SIGNAL_PEER, &obj, NULL);
 268 |     if (st != AB_OK) return st;
 269 | 
 270 |     st = ab_eventpair_signal_peer(obj, clear_mask, set_mask);
 271 |     ab_object_release(obj);
 272 |     return st;
 273 | }
 274 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 11: sys_object_wait_one (SYS_OBJECT_WAIT_ONE)
       //// --------------------------------------------------------------------------
       //// Synchronously blocks the calling thread until at least one bit in
       //// 'watched_signals' becomes active on 'handle' or 'deadline_ns' expires
       //// (0 = AB_TIME_INFINITE). Requires AB_RIGHT_WAIT.
       ////
       //// Notice user_range_valid() checks 'user_observed' BEFORE sleeping so a
       //// bogus kernel address is rejected immediately rather than after waiting.
       //// Also note that 'kobs' (the snapshot of observed signals) is copied out
       //// even when state_tracker_wait_one() returns AB_ERR_TIMED_OUT, matching
       //// Zircon's zx_object_wait_one() contract.
 275 | static ab_status_t sys_object_wait_one(ab_handle_t handle,
 276 |                                        ab_signals_t watched_signals,
 277 |                                        uint64_t deadline_ns,
 278 |                                        ab_signals_t *user_observed) {
 279 |     if (user_observed && !user_range_valid((uintptr_t)user_observed, sizeof(ab_signals_t))) {
 280 |         return AB_ERR_INVALID_ARGS;
 281 |     }
 282 |     ab_object_t *obj = NULL;
 283 |     ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
 284 |                                    AB_RIGHT_WAIT, &obj, NULL);
 285 |     if (st != AB_OK) return st;
 286 | 
 287 |     ab_signals_t kobs = 0;
 288 |     st = state_tracker_wait_one(obj, watched_signals, deadline_ns, &kobs);
 289 |     ab_object_release(obj);
 290 | 
 291 |     if (user_observed) {
 292 |         ab_status_t cst = copy_to_user(user_observed, &kobs, sizeof(kobs));
 293 |         if (cst != AB_OK) return cst;
 294 |     }
 295 |     return st;
 296 | }
 297 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 12: sys_port_create (SYS_PORT_CREATE)
       //// --------------------------------------------------------------------------
       //// Creates a PortDispatcher ring buffer (capacity 32 packets) for epoll/kqueue
       //// style asynchronous wait multiplexing across many kernel objects.
 298 | static ab_status_t sys_port_create(uint32_t options, ab_handle_t *user_out) {
 299 |     if (!user_out) return AB_ERR_INVALID_ARGS;
 300 |     ab_process_t *proc = get_current_process();
 301 |     ab_object_t *port = NULL;
 302 |     ab_status_t st = ab_port_dispatcher_create(options, &port);
 303 |     if (st != AB_OK) return st;
 304 | 
 305 |     ab_handle_t h = AB_HANDLE_INVALID;
 306 |     st = handle_alloc(proc, port, AB_RIGHTS_DEFAULT_PORT, &h);
 307 |     ab_object_release(port);
 308 |     if (st != AB_OK) return st;
 309 | 
 310 |     st = copy_to_user(user_out, &h, sizeof(h));
 311 |     if (st != AB_OK) {
 312 |         handle_close(proc, h);
 313 |     }
 314 |     return st;
 315 | }
 316 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 13: sys_port_queue (SYS_PORT_QUEUE)
       //// --------------------------------------------------------------------------
       //// Enqueues a user-crafted packet (forced to AB_PKT_TYPE_USER inside
       //// ab_port_queue) onto a Port, requiring AB_RIGHT_WRITE. Used by event
       //// loops to wake worker threads with custom notifications.
 317 | static ab_status_t sys_port_queue(ab_handle_t port_handle,
 318 |                                   const ab_port_packet_t *user_pkt) {
 319 |     if (!user_pkt) return AB_ERR_INVALID_ARGS;
 320 |     ab_port_packet_t kpkt;
 321 |     ab_status_t st = copy_from_user(&kpkt, user_pkt, sizeof(kpkt));
 322 |     if (st != AB_OK) return st;
 323 | 
 324 |     ab_object_t *port = NULL;
 325 |     st = handle_lookup(get_current_process(), port_handle,
 326 |                        AB_OBJ_TYPE_PORT, AB_RIGHT_WRITE, &port, NULL);
 327 |     if (st != AB_OK) return st;
 328 | 
 329 |     st = ab_port_queue(port, &kpkt);
 330 |     ab_object_release(port);
 331 |     return st;
 332 | }
 333 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 14: sys_port_wait (SYS_PORT_WAIT)
       //// --------------------------------------------------------------------------
       //// Dequeues the next packet from a Port (blocking until a packet arrives or
       //// deadline_ns elapses), requiring AB_RIGHT_READ. Copies the dequeued
       //// ab_port_packet_t back to EL0 on AB_OK.
 334 | static ab_status_t sys_port_wait(ab_handle_t port_handle,
 335 |                                  uint64_t deadline_ns,
 336 |                                  ab_port_packet_t *user_out_pkt) {
 337 |     if (!user_out_pkt || !user_range_valid((uintptr_t)user_out_pkt, sizeof(ab_port_packet_t))) {
 338 |         return AB_ERR_INVALID_ARGS;
 339 |     }
 340 |     ab_object_t *port = NULL;
 341 |     ab_status_t st = handle_lookup(get_current_process(), port_handle,
 342 |                                    AB_OBJ_TYPE_PORT, AB_RIGHT_READ, &port, NULL);
 343 |     if (st != AB_OK) return st;
 344 | 
 345 |     ab_port_packet_t kpkt;
 346 |     st = ab_port_wait(port, deadline_ns, &kpkt);
 347 |     ab_object_release(port);
 348 |     if (st != AB_OK) return st;
 349 | 
 350 |     return copy_to_user(user_out_pkt, &kpkt, sizeof(kpkt));
 351 | }
 352 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 15: sys_object_wait_async (SYS_OBJECT_WAIT_ASYNC)
       //// --------------------------------------------------------------------------
       //// Binds a one-shot asynchronous observer onto 'handle' (requires
       //// AB_RIGHT_WAIT) targeting 'port_handle' (requires AB_RIGHT_WRITE).
       //// When any bit in 'signals' is already asserted or later becomes asserted
       //// on 'obj', its StateTracker automatically enqueues an
       //// AB_PKT_TYPE_SIGNAL_ONE packet tagged with 'key' into 'port'.
 353 | static ab_status_t sys_object_wait_async(ab_handle_t handle,
 354 |                                          ab_handle_t port_handle,
 355 |                                          uint64_t key,
 356 |                                          ab_signals_t signals) {
 357 |     ab_process_t *proc = get_current_process();
 358 |     ab_object_t *obj = NULL;
 359 |     ab_status_t st = handle_lookup(proc, handle, AB_OBJ_TYPE_NONE,
 360 |                                    AB_RIGHT_WAIT, &obj, NULL);
 361 |     if (st != AB_OK) return st;
 362 | 
 363 |     ab_object_t *port = NULL;
 364 |     st = handle_lookup(proc, port_handle, AB_OBJ_TYPE_PORT,
 365 |                        AB_RIGHT_WRITE, &port, NULL);
 366 |     if (st != AB_OK) {
 367 |         ab_object_release(obj);
 368 |         return st;
 369 |     }
 370 | 
 371 |     st = state_tracker_wait_async(obj, port, key, signals);
 372 |     ab_object_release(port);
 373 |     ab_object_release(obj);
 374 |     return st;
 375 | }
 376 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 16: sys_channel_create (SYS_CHANNEL_CREATE)
       //// --------------------------------------------------------------------------
       //// Creates a bidirectional pair of ChannelDispatchers (ch0 <-> ch1) with
       //// AB_RIGHTS_DEFAULT_CHANNEL, following the same atomic dual-handle
       //// allocation and rollback discipline as sys_eventpair_create().
 377 | static ab_status_t sys_channel_create(uint32_t options,
 378 |                                       ab_handle_t *user_out0,
 379 |                                       ab_handle_t *user_out1) {
 380 |     if (options != 0 || !user_out0 || !user_out1) return AB_ERR_INVALID_ARGS;
 381 |     ab_process_t *proc = get_current_process();
 382 | 
 383 |     ab_object_t *ch0 = NULL, *ch1 = NULL;
 384 |     ab_status_t st = ab_channel_dispatcher_create(&ch0, &ch1);
 385 |     if (st != AB_OK) return st;
 386 | 
 387 |     ab_handle_t h0 = AB_HANDLE_INVALID, h1 = AB_HANDLE_INVALID;
 388 |     st = handle_alloc(proc, ch0, AB_RIGHTS_DEFAULT_CHANNEL, &h0);
 389 |     if (st == AB_OK) {
 390 |         st = handle_alloc(proc, ch1, AB_RIGHTS_DEFAULT_CHANNEL, &h1);
 391 |         if (st != AB_OK) handle_close(proc, h0);
 392 |     }
 393 |     ab_object_release(ch0);
 394 |     ab_object_release(ch1);
 395 |     if (st != AB_OK) return st;
 396 | 
 397 |     st = copy_to_user(user_out0, &h0, sizeof(h0));
 398 |     if (st == AB_OK) {
 399 |         st = copy_to_user(user_out1, &h1, sizeof(h1));
 400 |     }
 401 |     if (st != AB_OK) {
 402 |         handle_close(proc, h0);
 403 |         handle_close(proc, h1);
 404 |     }
 405 |     return st;
 406 | }
 407 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 17: sys_channel_write (SYS_CHANNEL_WRITE)
       //// --------------------------------------------------------------------------
       //// Sends a datagram of up to 256 bytes PLUS up to 8 capability handles
       //// across a Channel endpoint (requires AB_RIGHT_WRITE).
       ////
       //// TWO-STAGE ARGUMENT COPY (TOCTOU DEFENSE):
       //// Because sys_channel_write logically takes 6 parameters, user space packs
       //// {bytes, num_bytes, handles, num_handles} into ab_channel_write_args_t.
       ////   1. Copy the 32-byte descriptor struct into stack-local 'kargs' ONCE.
       ////   2. Validate kargs.num_bytes <= 256 and kargs.num_handles <= 8 against
       ////      that immutable kernel copy (so another EL0 thread racing on user
       ////      memory cannot mutate num_bytes after validation!).
       ////   3. Copy the payload bytes and source handle array onto the kernel stack
       ////      (kbytes[256] + khandles[8] = 288 bytes total, well within stack
       ////      budget) and invoke ab_channel_write(), which atomically moves each
       ////      handle (requiring AB_RIGHT_TRANSFER) out of the sender's table.
 408 | static ab_status_t sys_channel_write(ab_handle_t handle, uint32_t options,
 409 |                                      const ab_channel_write_args_t *user_args) {
 410 |     if (options != 0 || !user_args) return AB_ERR_INVALID_ARGS;
 411 | 
 412 |     ab_channel_write_args_t kargs;
 413 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 414 |     if (st != AB_OK) return st;
 415 | 
 416 |     if (kargs.num_bytes > CHANNEL_MAX_MSG_BYTES ||
 417 |         kargs.num_handles > CHANNEL_MAX_MSG_HANDLES) {
 418 |         return AB_ERR_OUT_OF_RANGE;
 419 |     }
 420 | 
 421 |     uint8_t kbytes[CHANNEL_MAX_MSG_BYTES];
 422 |     ab_handle_t khandles[CHANNEL_MAX_MSG_HANDLES];
 423 |     if (kargs.num_bytes > 0) {
 424 |         st = copy_from_user(kbytes, kargs.bytes, kargs.num_bytes);
 425 |         if (st != AB_OK) return st;
 426 |     }
 427 |     if (kargs.num_handles > 0) {
 428 |         st = copy_from_user(khandles, kargs.handles,
 429 |                             kargs.num_handles * sizeof(ab_handle_t));
 430 |         if (st != AB_OK) return st;
 431 |     }
 432 | 
 433 |     ab_process_t *proc = get_current_process();
 434 |     ab_object_t *ch = NULL;
 435 |     st = handle_lookup(proc, handle, AB_OBJ_TYPE_CHANNEL,
 436 |                        AB_RIGHT_WRITE, &ch, NULL);
 437 |     if (st != AB_OK) return st;
 438 | 
 439 |     st = ab_channel_write(proc, ch,
 440 |                           (kargs.num_bytes > 0) ? kbytes : NULL, kargs.num_bytes,
 441 |                           (kargs.num_handles > 0) ? khandles : NULL, kargs.num_handles);
 442 |     ab_object_release(ch);
 443 |     return st;
 444 | }
 445 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 18: sys_channel_read (SYS_CHANNEL_READ)
       //// --------------------------------------------------------------------------
       //// Dequeues the oldest message from a Channel endpoint (non-blocking;
       //// returns AB_ERR_SHOULD_WAIT if empty or AB_ERR_PEER_CLOSED if empty and
       //// peer closed), installing any attached objects as brand-new handles in
       //// the receiver's handle table!
 446 | static ab_status_t sys_channel_read(ab_handle_t handle, uint32_t options,
 447 |                                     ab_channel_read_args_t *user_args) {
 448 |     if (options != 0 || !user_args) return AB_ERR_INVALID_ARGS;
 449 | 
 450 |     ab_channel_read_args_t kargs;
 451 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 452 |     if (st != AB_OK) return st;
 453 | 
       //// Clamp max_bytes and max_handles to the kernel maxima and pre-validate
       //// the user destination buffers via user_range_valid() before dequeuing.
 454 |     uint32_t max_b = (kargs.max_bytes > CHANNEL_MAX_MSG_BYTES)
 455 |                          ? CHANNEL_MAX_MSG_BYTES : kargs.max_bytes;
 456 |     uint32_t max_h = (kargs.max_handles > CHANNEL_MAX_MSG_HANDLES)
 457 |                          ? CHANNEL_MAX_MSG_HANDLES : kargs.max_handles;
 458 |     if ((max_b > 0 && !user_range_valid((uintptr_t)kargs.bytes, max_b)) ||
 459 |         (max_h > 0 && !user_range_valid((uintptr_t)kargs.handles, max_h * sizeof(ab_handle_t)))) {
 460 |         return AB_ERR_INVALID_ARGS;
 461 |     }
 462 | 
 463 |     ab_process_t *proc = get_current_process();
 464 |     ab_object_t *ch = NULL;
 465 |     st = handle_lookup(proc, handle, AB_OBJ_TYPE_CHANNEL,
 466 |                        AB_RIGHT_READ, &ch, NULL);
 467 |     if (st != AB_OK) return st;
 468 | 
 469 |     uint8_t kbytes[CHANNEL_MAX_MSG_BYTES];
 470 |     ab_handle_t khandles[CHANNEL_MAX_MSG_HANDLES];
 471 |     uint32_t act_b = 0, act_h = 0;
 472 | 
 473 |     st = ab_channel_read(proc, ch, kbytes, khandles, max_b, max_h, &act_b, &act_h);
 474 |     ab_object_release(ch);
 475 | 
       //// SIZE-PROBING & HANDLE ROLLBACK:
       ////   - On AB_OK or AB_ERR_BUFFER_TOO_SMALL, report actual_bytes and
       ////     actual_handles back to EL0 so callers can size buffers dynamically.
       ////   - On AB_OK, ab_channel_read() has ALREADY consumed the message packet
       ////     and installed act_h new handles in 'proc'. If copying either the
       ////     message bytes or the handle array into EL0 memory fails (lines
       ////     488-493), lines 495-497 close all act_h newly minted handles so
       ////     capabilities are cleanly destroyed rather than leaked in 'proc'!
 476 |     if (st == AB_OK || st == AB_ERR_BUFFER_TOO_SMALL) {
 477 |         if (kargs.actual_bytes) {
 478 |             ab_status_t cst = copy_to_user(kargs.actual_bytes, &act_b, sizeof(act_b));
 479 |             if (cst != AB_OK && st == AB_OK) st = cst;
 480 |         }
 481 |         if (kargs.actual_handles) {
 482 |             ab_status_t cst = copy_to_user(kargs.actual_handles, &act_h, sizeof(act_h));
 483 |             if (cst != AB_OK && st == AB_OK) st = cst;
 484 |         }
 485 |     }
 486 | 
 487 |     if (st == AB_OK) {
 488 |         if (act_b > 0) {
 489 |             st = copy_to_user(kargs.bytes, kbytes, act_b);
 490 |         }
 491 |         if (st == AB_OK && act_h > 0) {
 492 |             st = copy_to_user(kargs.handles, khandles, act_h * sizeof(ab_handle_t));
 493 |         }
 494 |         if (st != AB_OK) {
 495 |             for (uint32_t i = 0; i < act_h; i++) {
 496 |                 handle_close(proc, khandles[i]);
 497 |             }
 498 |         }
 499 |     }
 500 |     return st;
 501 | }
 502 | 
```

### 11.3.3 Technical Notes

1. **Reference-Counting Discipline Around `handle_lookup()`**: Every successful `handle_lookup()` call atomically increments `obj->ref_count` while holding `proc->handle_lock`. Even if another thread in the same user process concurrently calls `sys_handle_close(handle)` on another CPU core while a blocking call like `sys_object_wait_one` or `sys_port_wait` is asleep, the underlying `ab_object_t` remains valid in kernel heap memory until the syscall wakes up and calls `ab_object_release(obj)`.
2. **Protecting Kernel-Owned Signals (`USER_ALLOWED_SIGNALS`)**: Allowing `EL0` to clear `AB_SIGNAL_PEER_CLOSED` or assert `AB_SIGNAL_READABLE` on a `ChannelDispatcher` would let user code spoof IPC state or desynchronize `StateTracker` bits from `ch->msg_count`. Restricting `sys_object_signal` and `sys_object_signal_peer` to `AB_SIGNAL_SIGNALED | AB_USER_SIGNAL_ALL` cleanly separates kernel-driven readiness signals from application-defined synchronization signals.
3. **Channel Handle Move Semantics & Failure Recovery**: In Zircon and ABOS, writing handles into a channel (`sys_channel_write` -> `handle_take_for_transfer`) *moves* rather than copies them: each source handle requires `AB_RIGHT_TRANSFER` and is atomically removed from the sender's handle table as the `ab_message_packet_t` takes ownership of the `ab_object_t *` reference. Conversely, when `sys_channel_read` installs those objects into the receiver's handle table, a subsequent `copy_to_user` failure on lines 488–493 triggers the cleanup loop on lines 495–497 (`handle_close(proc, khandles[i])`), upholding Zircon's invariant that failed channel operations consume rather than leak capabilities.

---

## 11.4 VMO, VMAR, Task, Driver DDK and Introspection Syscalls

### 11.4.1 Functional Description

Lines 503–971 implement the remaining 17 object-oriented system calls across four higher-level subsystems:
1. **Virtual Memory Objects & Address Space Regions** (`sys_vmo_create`, `sys_vmo_read`, `sys_vmo_write`, `sys_vmar_map`, `sys_vmar_unmap`): Creating demand-paged VMOs, performing chunked 256-byte read/write I/O without mapping VMOs into the caller's `VmAspace` (ideal when a parent loader populates a child's code/data VMOs!), and mapping/unmapping VMO slices inside a target process's `VMAR`.
2. **Zircon Three-Phase Task Creation** (`sys_process_create`, `sys_thread_create`, `sys_process_start`): Replacing UNIX `fork()`/`execve()` with capability-delegated construction of an empty `ProcessDispatcher` + root `VMAR` under a `Job`, creation of a `ThreadDispatcher`, and explicit `EL0` startup with a transferred bootstrap handle in `x0`.
3. **User-Space Device Driver Kit (DDK)** (`sys_vmo_create_physical`, `sys_vmo_create_contiguous`, `sys_interrupt_create`, `sys_interrupt_wait`, `sys_interrupt_ack`): Gated behind `AB_OBJ_TYPE_RESOURCE`, these five syscalls empower `EL0` servers (`console-srv` and `fs-srv`) to map MMIO registers, allocate physically contiguous DMA rings/buffers, and service GIC hardware interrupts entirely in user space.
4. **System Diagnostics & Introspection** (`sys_object_set_name`, `sys_job_get_ps_info`, `sys_process_get_maps`): Assigning human-readable names to kernel objects and exporting structured snapshots of the job/process/thread tree (`ps`) and virtual memory hierarchy (`vmaps`) using heap-allocated staging buffers.

### 11.4.2 Heavily Commented Source Code

#### Program 11-3: `lesson_20/kernel/core/syscalls.c` (Lines 503–971)

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/syscalls.c" start="503" end="971" total="1219" -->
```c
       //// --------------------------------------------------------------------------
       //// SYSCALL 19: sys_vmo_create (SYS_VMO_CREATE)
       //// --------------------------------------------------------------------------
       //// Creates an anonymous Virtual Memory Object (VMO) rounded up to whole
       //// 4 KB pages (up to VMO_MAX_PAGES = 128 -> 512 KB). Physical RAM pages
       //// are NOT allocated upfront; they are committed lazily on first
       //// sys_vmo_write() or EL0 VMAR page fault.
 503 | static ab_status_t sys_vmo_create(uint64_t size, uint32_t options,
 504 |                                   ab_handle_t *user_out) {
 505 |     if (!user_out) return AB_ERR_INVALID_ARGS;
 506 |     ab_process_t *proc = get_current_process();
 507 |     ab_object_t *vmo = NULL;
 508 |     ab_status_t st = ab_vmo_dispatcher_create((size_t)size, options, &vmo);
 509 |     if (st != AB_OK) return st;
 510 | 
 511 |     ab_handle_t h = AB_HANDLE_INVALID;
 512 |     st = handle_alloc(proc, vmo, AB_RIGHTS_DEFAULT_VMO, &h);
 513 |     ab_object_release(vmo);
 514 |     if (st != AB_OK) return st;
 515 | 
 516 |     st = copy_to_user(user_out, &h, sizeof(h));
 517 |     if (st != AB_OK) {
 518 |         handle_close(proc, h);
 519 |     }
 520 |     return st;
 521 | }
 522 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 20: sys_vmo_read (SYS_VMO_READ)
       //// --------------------------------------------------------------------------
       //// Reads 'len' bytes starting at 'offset' from a VMO (requires
       //// AB_RIGHT_READ) into 'user_buf'.
       ////
       //// Rather than allocating an arbitrarily large heap buffer, sys_vmo_read()
       //// streams data through a tiny 256-byte stack bounce buffer (kchunk[256]),
       //// keeping stack usage tiny and latency preemptible across large transfers.
 523 | static ab_status_t sys_vmo_read(ab_handle_t handle, void *user_buf,
 524 |                                 uint64_t offset, size_t len) {
 525 |     if (len == 0) return AB_OK;
 526 |     if (!user_buf || !user_range_valid((uintptr_t)user_buf, len)) {
 527 |         return AB_ERR_INVALID_ARGS;
 528 |     }
 529 |     ab_object_t *vmo = NULL;
 530 |     ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_VMO,
 531 |                                    AB_RIGHT_READ, &vmo, NULL);
 532 |     if (st != AB_OK) return st;
 533 | 
 534 |     uint8_t kchunk[256];
 535 |     size_t done = 0;
 536 |     while (done < len) {
 537 |         size_t chunk = (len - done > sizeof(kchunk)) ? sizeof(kchunk) : (len - done);
 538 |         st = ab_vmo_read(vmo, kchunk, offset + done, chunk);
 539 |         if (st != AB_OK) break;
 540 |         st = copy_to_user((uint8_t *)user_buf + done, kchunk, chunk);
 541 |         if (st != AB_OK) break;
 542 |         done += chunk;
 543 |     }
 544 | 
 545 |     ab_object_release(vmo);
 546 |     return st;
 547 | }
 548 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 21: sys_vmo_write (SYS_VMO_WRITE)
       //// --------------------------------------------------------------------------
       //// Writes 'len' bytes from 'user_buf' into a VMO at 'offset' (requires
       //// AB_RIGHT_WRITE), streaming in 256-byte chunks. Uncommitted VMO pages
       //// touched by ab_vmo_write() are allocated on demand from the PMM.
 549 | static ab_status_t sys_vmo_write(ab_handle_t handle, const void *user_buf,
 550 |                                  uint64_t offset, size_t len) {
 551 |     if (len == 0) return AB_OK;
 552 |     if (!user_buf || !user_range_valid((uintptr_t)user_buf, len)) {
 553 |         return AB_ERR_INVALID_ARGS;
 554 |     }
 555 |     ab_object_t *vmo = NULL;
 556 |     ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_VMO,
 557 |                                    AB_RIGHT_WRITE, &vmo, NULL);
 558 |     if (st != AB_OK) return st;
 559 | 
 560 |     uint8_t kchunk[256];
 561 |     size_t done = 0;
 562 |     while (done < len) {
 563 |         size_t chunk = (len - done > sizeof(kchunk)) ? sizeof(kchunk) : (len - done);
 564 |         st = copy_from_user(kchunk, (const uint8_t *)user_buf + done, chunk);
 565 |         if (st != AB_OK) break;
 566 |         st = ab_vmo_write(vmo, kchunk, offset + done, chunk);
 567 |         if (st != AB_OK) break;
 568 |         done += chunk;
 569 |     }
 570 | 
 571 |     ab_object_release(vmo);
 572 |     return st;
 573 | }
 574 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 22: sys_vmar_map (SYS_VMAR_MAP)
       //// --------------------------------------------------------------------------
       //// Maps a slice of a VMO into a Virtual Memory Address Region (VMAR).
       //// Because mapping requires 7 logical parameters, EL0 passes a pointer to
       //// ab_vmar_map_args_t {vmo_handle, vmo_offset, len, map_flags, mapped_va_out}.
 575 | static ab_status_t sys_vmar_map(ab_handle_t vmar_handle, size_t vmar_offset,
 576 |                                 const ab_vmar_map_args_t *user_args) {
 577 |     if (!user_args) return AB_ERR_INVALID_ARGS;
 578 |     ab_vmar_map_args_t kargs;
 579 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 580 |     if (st != AB_OK) return st;
 581 |     if (!kargs.mapped_va_out) return AB_ERR_INVALID_ARGS;
 582 | 
 583 |     ab_process_t *proc = get_current_process();
 584 |     ab_object_t *vmar = NULL;
 585 |     st = handle_lookup(proc, vmar_handle, AB_OBJ_TYPE_VMAR,
 586 |                        AB_RIGHT_MAP, &vmar, NULL);
 587 |     if (st != AB_OK) return st;
 588 | 
       //// DUAL CAPABILITY ENFORCEMENT (VMAR + VMO):
       //// Mapping memory is a two-object operation:
       ////   1. The caller must hold AB_RIGHT_MAP on the target VMAR (line 586).
       ////   2. The caller must ALSO hold AB_RIGHT_MAP on the backing VMO PLUS
       ////      every individual permission bit requested in map_flags:
       ////        AB_VM_PERM_READ    -> requires AB_RIGHT_READ on VMO
       ////        AB_VM_PERM_WRITE   -> requires AB_RIGHT_WRITE on VMO
       ////        AB_VM_PERM_EXECUTE -> requires AB_RIGHT_EXECUTE on VMO
       ////
       //// Why this matters: When lk_main hands userboot the shared bootfs.tar VMO,
       //// it strips AB_RIGHT_WRITE (bootfs_ro_rights = BASIC | READ | MAP). If a
       //// compromised EL0 server attempts sys_vmar_map(..., AB_VM_PERM_WRITE) on
       //// bootfs.tar, handle_lookup() on line 596 rejects it with
       //// AB_ERR_ACCESS_DENIED! And if copy_to_user() of mapped_va_out fails on
       //// line 608, ab_vmar_unmap() immediately tears down the mapping.
 589 |     /* Enforce capability rights on the VMO being mapped */
 590 |     ab_rights_t req_vmo_rights = AB_RIGHT_MAP;
 591 |     if (kargs.map_flags & AB_VM_PERM_READ)    req_vmo_rights |= AB_RIGHT_READ;
 592 |     if (kargs.map_flags & AB_VM_PERM_WRITE)   req_vmo_rights |= AB_RIGHT_WRITE;
 593 |     if (kargs.map_flags & AB_VM_PERM_EXECUTE) req_vmo_rights |= AB_RIGHT_EXECUTE;
 594 | 
 595 |     ab_object_t *vmo = NULL;
 596 |     st = handle_lookup(proc, kargs.vmo_handle, AB_OBJ_TYPE_VMO,
 597 |                        req_vmo_rights, &vmo, NULL);
 598 |     if (st != AB_OK) {
 599 |         ab_object_release(vmar);
 600 |         return st;
 601 |     }
 602 | 
 603 |     vaddr_t mapped_va = 0;
 604 |     st = ab_vmar_map(proc, vmar, vmar_offset, vmo,
 605 |                      kargs.vmo_offset, (size_t)kargs.len,
 606 |                      kargs.map_flags, &mapped_va);
 607 |     if (st == AB_OK) {
 608 |         st = copy_to_user(kargs.mapped_va_out, &mapped_va, sizeof(mapped_va));
 609 |         if (st != AB_OK) {
 610 |             ab_vmar_unmap(proc, vmar, mapped_va, (size_t)kargs.len);
 611 |         }
 612 |     }
 613 | 
 614 |     ab_object_release(vmo);
 615 |     ab_object_release(vmar);
 616 |     return st;
 617 | }
 618 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 34: sys_vmar_unmap (SYS_VMAR_UNMAP)
       //// --------------------------------------------------------------------------
       //// Unmaps active VMAR mappings within [addr, addr + len), flushing ASID-tagged
       //// TLB entries across all SMP cores via tlbi vae1is and releasing each
       //// mapping's reference on its backing VMO. Requires AB_RIGHT_MAP on VMAR.
 619 | static ab_status_t sys_vmar_unmap(ab_handle_t vmar_handle, vaddr_t addr, size_t len) {
 620 |     ab_process_t *proc = get_current_process();
 621 |     ab_object_t *vmar = NULL;
 622 |     ab_status_t st = handle_lookup(proc, vmar_handle, AB_OBJ_TYPE_VMAR,
 623 |                                    AB_RIGHT_MAP, &vmar, NULL);
 624 |     if (st != AB_OK) return st;
 625 | 
 626 |     st = ab_vmar_unmap(proc, vmar, addr, len);
 627 |     ab_object_release(vmar);
 628 |     return st;
 629 | }
 630 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 23: sys_process_create (SYS_PROCESS_CREATE)
       //// --------------------------------------------------------------------------
       //// Step 1 of Zircon's explicit 3-step process creation model (replacing
       //// POSIX fork() + execve()). To spawn a child process, the caller cannot
       //// simply clone itself; it must hold a Job capability with
       //// AB_RIGHT_MANAGE_PROCESS (lines 649-650).
 631 | static ab_status_t sys_process_create(ab_handle_t job_handle,
 632 |                                       const ab_process_create_args_t *user_args) {
 633 |     if (!user_args) return AB_ERR_INVALID_ARGS;
 634 |     ab_process_create_args_t kargs;
 635 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 636 |     if (st != AB_OK) return st;
 637 |     if (!kargs.name || !kargs.proc_handle_out || !kargs.vmar_handle_out) {
 638 |         return AB_ERR_INVALID_ARGS;
 639 |     }
 640 | 
 641 |     char kname[32];
 642 |     size_t n = (kargs.name_len < 31) ? kargs.name_len : 31;
 643 |     st = copy_from_user(kname, kargs.name, n);
 644 |     if (st != AB_OK) return st;
 645 |     kname[n] = '\0';
 646 | 
 647 |     ab_process_t *caller = get_current_process();
 648 |     ab_object_t *job_obj = NULL;
 649 |     st = handle_lookup(caller, job_handle, AB_OBJ_TYPE_JOB,
 650 |                        AB_RIGHT_MANAGE_PROCESS, &job_obj, NULL);
 651 |     if (st != AB_OK) return st;
 652 | 
       //// Construct a fresh empty ProcessDispatcher (allocating a hardware ASID
       //// and empty TTBR0_EL1 L1 page table) plus its root VmarDispatcher, mint
       //// both proc_h and vmar_h into the parent's handle table so the parent can
       //// populate the child's address space remotely via sys_vmar_map(), attach
       //// new_proc to job_obj ONLY after both copy_to_user() calls succeed, and
       //// drop the three local kernel object references.
 653 |     ab_process_t *new_proc = ab_process_create_kernel(kname, ab_alloc_asid());
 654 |     if (!new_proc) {
 655 |         ab_object_release(job_obj);
 656 |         return AB_ERR_NO_MEMORY;
 657 |     }
 658 | 
 659 |     ab_object_t *vmar_obj = NULL;
 660 |     st = ab_vmar_dispatcher_create(new_proc, &vmar_obj);
 661 |     if (st != AB_OK) {
 662 |         ab_object_release(&new_proc->base);
 663 |         ab_object_release(job_obj);
 664 |         return st;
 665 |     }
 666 | 
 667 |     ab_handle_t proc_h = AB_HANDLE_INVALID, vmar_h = AB_HANDLE_INVALID;
 668 |     st = handle_alloc(caller, &new_proc->base, AB_RIGHTS_DEFAULT_PROCESS, &proc_h);
 669 |     if (st == AB_OK) {
 670 |         st = handle_alloc(caller, vmar_obj, AB_RIGHTS_DEFAULT_VMAR, &vmar_h);
 671 |         if (st != AB_OK) handle_close(caller, proc_h);
 672 |     }
 673 | 
 674 |     if (st == AB_OK) {
 675 |         st = copy_to_user(kargs.proc_handle_out, &proc_h, sizeof(proc_h));
 676 |         if (st == AB_OK) {
 677 |             st = copy_to_user(kargs.vmar_handle_out, &vmar_h, sizeof(vmar_h));
 678 |         }
 679 |         if (st == AB_OK) {
 680 |             ab_job_add_process((ab_job_dispatcher_t *)job_obj, new_proc);
 681 |         } else {
 682 |             handle_close(caller, proc_h);
 683 |             handle_close(caller, vmar_h);
 684 |         }
 685 |     }
 686 | 
 687 |     ab_object_release(vmar_obj);
 688 |     ab_object_release(&new_proc->base);
 689 |     ab_object_release(job_obj);
 690 |     return st;
 691 | }
 692 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 24: sys_thread_create (SYS_THREAD_CREATE)
       //// --------------------------------------------------------------------------
       //// Step 2 of task creation: creates an initial Dormant ThreadDispatcher
       //// inside 'proc_handle', requiring AB_RIGHT_MANAGE_THREAD on the process.
       //// No EL1 scheduler thread runs yet until sys_process_start() is invoked.
 693 | static ab_status_t sys_thread_create(ab_handle_t proc_handle,
 694 |                                      const char *user_name, size_t name_len,
 695 |                                      ab_handle_t *user_out) {
 696 |     if (!user_name || !user_out) return AB_ERR_INVALID_ARGS;
 697 | 
 698 |     char kname[32];
 699 |     size_t n = (name_len < 31) ? name_len : 31;
 700 |     ab_status_t st = copy_from_user(kname, user_name, n);
 701 |     if (st != AB_OK) return st;
 702 |     kname[n] = '\0';
 703 | 
 704 |     ab_process_t *caller = get_current_process();
 705 |     ab_object_t *proc_obj = NULL;
 706 |     st = handle_lookup(caller, proc_handle, AB_OBJ_TYPE_PROCESS,
 707 |                        AB_RIGHT_MANAGE_THREAD, &proc_obj, NULL);
 708 |     if (st != AB_OK) return st;
 709 | 
 710 |     ab_object_t *td_obj = NULL;
 711 |     st = ab_thread_dispatcher_create((ab_process_t *)proc_obj, kname, &td_obj);
 712 |     if (st == AB_OK) {
 713 |         ab_handle_t thr_h = AB_HANDLE_INVALID;
 714 |         st = handle_alloc(caller, td_obj, AB_RIGHTS_DEFAULT_THREAD, &thr_h);
 715 |         if (st == AB_OK) {
 716 |             st = copy_to_user(user_out, &thr_h, sizeof(thr_h));
 717 |             if (st != AB_OK) handle_close(caller, thr_h);
 718 |         }
 719 |         ab_object_release(td_obj);
 720 |     }
 721 |     ab_object_release(proc_obj);
 722 |     return st;
 723 | }
 724 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 25: sys_process_start (SYS_PROCESS_START)
       //// --------------------------------------------------------------------------
       //// Step 3 of task creation: ignites execution of a newly created process
       //// (requires AB_RIGHT_WRITE on both proc_handle and thread_handle).
       //// ab_process_start_user() atomically transfers 'kargs.arg_handle' (typically
       //// the child's bootstrap Channel endpoint) from the caller's handle table
       //// into slot 0 of the child process, sets up the EL0 entry PC, SP, x0
       //// (transferred handle value), and x1 (arg2), and marks the LK thread READY.
 725 | static ab_status_t sys_process_start(ab_handle_t proc_handle,
 726 |                                      const ab_process_start_args_t *user_args) {
 727 |     if (!user_args) return AB_ERR_INVALID_ARGS;
 728 |     ab_process_start_args_t kargs;
 729 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 730 |     if (st != AB_OK) return st;
 731 | 
 732 |     ab_process_t *caller = get_current_process();
 733 |     ab_object_t *proc_obj = NULL;
 734 |     st = handle_lookup(caller, proc_handle, AB_OBJ_TYPE_PROCESS,
 735 |                        AB_RIGHT_WRITE, &proc_obj, NULL);
 736 |     if (st != AB_OK) return st;
 737 | 
 738 |     ab_object_t *thr_obj = NULL;
 739 |     st = handle_lookup(caller, kargs.thread_handle, AB_OBJ_TYPE_THREAD,
 740 |                        AB_RIGHT_WRITE, &thr_obj, NULL);
 741 |     if (st != AB_OK) {
 742 |         ab_object_release(proc_obj);
 743 |         return st;
 744 |     }
 745 | 
 746 |     st = ab_process_start_user(caller, (ab_process_t *)proc_obj,
 747 |                                (ab_thread_dispatcher_t *)thr_obj,
 748 |                                kargs.entry_pc, kargs.user_sp,
 749 |                                kargs.arg_handle, kargs.arg2);
 750 |     ab_object_release(thr_obj);
 751 |     ab_object_release(proc_obj);
 752 |     return st;
 753 | }
 754 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 26: sys_vmo_create_physical (SYS_VMO_CREATE_PHYSICAL)
       //// --------------------------------------------------------------------------
       //// USER-SPACE DRIVER DDK GATE #1 (Device MMIO):
       //// Allows a privileged user-space driver (such as console-srv driving the
       //// PL011 UART or fs-srv driving virtio-blk) to wrap a hardware MMIO
       //// physical range [paddr, paddr + size) in a VMO marked is_physical = 1
       //// (mapped with MAIR Device-nGnRnE + UXN/PXN).
       ////
       //// Access is gated by requiring an AB_OBJ_TYPE_RESOURCE capability with
       //// AB_RIGHT_WRITE — unprivileged shells and apps never receive a Resource
       //// handle and therefore cannot map physical hardware!
 755 | static ab_status_t sys_vmo_create_physical(ab_handle_t resource_handle,
 756 |                                            const ab_vmo_create_phys_args_t *user_args) {
 757 |     if (!user_args) return AB_ERR_INVALID_ARGS;
 758 |     ab_vmo_create_phys_args_t kargs;
 759 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 760 |     if (st != AB_OK) return st;
 761 |     if (!kargs.vmo_out) return AB_ERR_INVALID_ARGS;
 762 | 
 763 |     ab_process_t *caller = get_current_process();
 764 |     ab_object_t *res_obj = NULL;
 765 |     st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
 766 |                        AB_RIGHT_WRITE, &res_obj, NULL);
 767 |     if (st != AB_OK) return st;
 768 |     ab_object_release(res_obj);
 769 | 
 770 |     ab_object_t *vmo_obj = NULL;
 771 |     st = ab_vmo_create_physical(kargs.paddr, kargs.size, &vmo_obj);
 772 |     if (st != AB_OK) return st;
 773 | 
 774 |     ab_handle_t vmo_h = AB_HANDLE_INVALID;
 775 |     st = handle_alloc(caller, vmo_obj, AB_RIGHTS_DEFAULT_VMO, &vmo_h);
 776 |     ab_object_release(vmo_obj);
 777 |     if (st != AB_OK) return st;
 778 | 
 779 |     st = copy_to_user(kargs.vmo_out, &vmo_h, sizeof(vmo_h));
 780 |     if (st != AB_OK) {
 781 |         handle_close(caller, vmo_h);
 782 |     }
 783 |     return st;
 784 | }
 785 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 27: sys_vmo_create_contiguous (SYS_VMO_CREATE_CONTIG)
       //// --------------------------------------------------------------------------
       //// USER-SPACE DRIVER DDK GATE #2 (Contiguous DMA Memory):
       //// Virtio-blk descriptor rings and DMA sector buffers require physically
       //// contiguous RAM pages AND need to program their physical bus address
       //// into device registers. Gated by AB_OBJ_TYPE_RESOURCE + AB_RIGHT_WRITE,
       //// this call allocates contiguous PMM pages and exports both the VMO
       //// handle (*vmo_out) and base physical address (*paddr_out) to the driver.
 786 | static ab_status_t sys_vmo_create_contiguous(ab_handle_t resource_handle,
 787 |                                              const ab_vmo_create_contig_args_t *user_args) {
 788 |     if (!user_args) return AB_ERR_INVALID_ARGS;
 789 |     ab_vmo_create_contig_args_t kargs;
 790 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 791 |     if (st != AB_OK) return st;
 792 |     if (!kargs.vmo_out || !kargs.paddr_out) return AB_ERR_INVALID_ARGS;
 793 | 
 794 |     ab_process_t *caller = get_current_process();
 795 |     ab_object_t *res_obj = NULL;
 796 |     st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
 797 |                        AB_RIGHT_WRITE, &res_obj, NULL);
 798 |     if (st != AB_OK) return st;
 799 |     ab_object_release(res_obj);
 800 | 
 801 |     ab_object_t *vmo_obj = NULL;
 802 |     paddr_t pa = 0;
 803 |     st = ab_vmo_create_contiguous(kargs.size, &vmo_obj, &pa);
 804 |     if (st != AB_OK) return st;
 805 | 
 806 |     ab_handle_t vmo_h = AB_HANDLE_INVALID;
 807 |     st = handle_alloc(caller, vmo_obj, AB_RIGHTS_DEFAULT_VMO, &vmo_h);
 808 |     ab_object_release(vmo_obj);
 809 |     if (st != AB_OK) return st;
 810 | 
 811 |     st = copy_to_user(kargs.paddr_out, &pa, sizeof(pa));
 812 |     if (st == AB_OK) {
 813 |         st = copy_to_user(kargs.vmo_out, &vmo_h, sizeof(vmo_h));
 814 |     }
 815 |     if (st != AB_OK) {
 816 |         handle_close(caller, vmo_h);
 817 |     }
 818 |     return st;
 819 | }
 820 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 28: sys_interrupt_create (SYS_INTERRUPT_CREATE)
       //// --------------------------------------------------------------------------
       //// USER-SPACE DRIVER DDK GATE #3 (Hardware IRQ Binding):
       //// Requires AB_OBJ_TYPE_RESOURCE with AB_RIGHT_READ to bind hardware GIC
       //// interrupt ID 'irq_num' (e.g., UART0 SPI 33 or virtio SPI) to an
       //// InterruptDispatcher object and unmask it in the GIC Distributor.
 821 | static ab_status_t sys_interrupt_create(ab_handle_t resource_handle,
 822 |                                         uint32_t irq_num,
 823 |                                         ab_handle_t *user_out) {
 824 |     if (!user_out) return AB_ERR_INVALID_ARGS;
 825 |     ab_process_t *caller = get_current_process();
 826 | 
 827 |     ab_object_t *res_obj = NULL;
 828 |     ab_status_t st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
 829 |                                    AB_RIGHT_READ, &res_obj, NULL);
 830 |     if (st != AB_OK) return st;
 831 |     ab_object_release(res_obj);
 832 | 
 833 |     ab_object_t *intr_obj = NULL;
 834 |     st = ab_interrupt_dispatcher_create(irq_num, &intr_obj);
 835 |     if (st != AB_OK) return st;
 836 | 
 837 |     ab_handle_t intr_h = AB_HANDLE_INVALID;
 838 |     st = handle_alloc(caller, intr_obj, AB_RIGHTS_DEFAULT_INTERRUPT, &intr_h);
 839 |     ab_object_release(intr_obj);
 840 |     if (st != AB_OK) return st;
 841 | 
 842 |     st = copy_to_user(user_out, &intr_h, sizeof(intr_h));
 843 |     if (st != AB_OK) {
 844 |         handle_close(caller, intr_h);
 845 |     }
 846 |     return st;
 847 | }
 848 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 29 & 30: sys_interrupt_wait & sys_interrupt_ack
       //// --------------------------------------------------------------------------
       //// User-space IRQ bottom-half loop:
       ////   - sys_interrupt_wait() (requires AB_RIGHT_WAIT) blocks the driver
       ////     thread until arm64_irq_exception() masks the GIC line and signals
       ////     intr_obj->irq_event.
       ////   - After servicing device registers in EL0, the driver calls
       ////     sys_interrupt_ack() (requires AB_RIGHT_WRITE) to re-enable the IRQ
       ////     line in GICD_ISENABLER for the next hardware interrupt.
 849 | static ab_status_t sys_interrupt_wait(ab_handle_t intr_handle) {
 850 |     ab_object_t *intr_obj = NULL;
 851 |     ab_status_t st = handle_lookup(get_current_process(), intr_handle,
 852 |                                    AB_OBJ_TYPE_INTERRUPT, AB_RIGHT_WAIT,
 853 |                                    &intr_obj, NULL);
 854 |     if (st != AB_OK) return st;
 855 |     st = ab_interrupt_wait(intr_obj);
 856 |     ab_object_release(intr_obj);
 857 |     return st;
 858 | }
 859 | 
 860 | static ab_status_t sys_interrupt_ack(ab_handle_t intr_handle) {
 861 |     ab_object_t *intr_obj = NULL;
 862 |     ab_status_t st = handle_lookup(get_current_process(), intr_handle,
 863 |                                    AB_OBJ_TYPE_INTERRUPT, AB_RIGHT_WRITE,
 864 |                                    &intr_obj, NULL);
 865 |     if (st != AB_OK) return st;
 866 |     st = ab_interrupt_ack(intr_obj);
 867 |     ab_object_release(intr_obj);
 868 |     return st;
 869 | }
 870 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 31: sys_object_set_name (SYS_OBJECT_SET_NAME)
       //// --------------------------------------------------------------------------
       //// Attaches a 31-character NUL-terminated diagnostic label (such as
       //// '.text', '.rodata', 'heap', or 'stack') to a VMO, Process, Thread, or
       //// Job so introspection tools ('ps' and 'vmaps') display meaningful names.
 871 | static ab_status_t sys_object_set_name(ab_handle_t handle, const char *user_name, size_t len) {
 872 |     if (!user_name) return AB_ERR_INVALID_ARGS;
 873 |     char kname[32];
 874 |     size_t n = (len < 31) ? len : 31;
 875 |     ab_status_t st = copy_from_user(kname, user_name, n);
 876 |     if (st != AB_OK) return st;
 877 |     kname[n] = '\0';
 878 | 
 879 |     ab_object_t *obj = NULL;
 880 |     st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
 881 |                        AB_RIGHT_NONE, &obj, NULL);
 882 |     if (st != AB_OK) return st;
 883 |     st = ab_object_set_name(obj, kname, n);
 884 |     ab_object_release(obj);
 885 |     return st;
 886 | }
 887 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 32: sys_job_get_ps_info (SYS_JOB_GET_PS_INFO)
       //// --------------------------------------------------------------------------
       //// Backs the interactive 'ps' shell command. Requires AB_OBJ_TYPE_JOB with
       //// AB_RIGHT_INSPECT so a process can only inspect tasks governed by a Job
       //// capability it holds.
 888 | static ab_status_t sys_job_get_ps_info(ab_handle_t job_handle,
 889 |                                        ab_task_info_entry_t *user_entries,
 890 |                                        uint32_t max_entries,
 891 |                                        uint32_t *user_actual) {
 892 |     if (!user_entries || !user_actual || max_entries == 0) {
 893 |         return AB_ERR_INVALID_ARGS;
 894 |     }
 895 |     uint32_t capped = (max_entries > 64U) ? 64U : max_entries;
 896 |     if (!user_range_valid((uintptr_t)user_entries, capped * sizeof(ab_task_info_entry_t)) ||
 897 |         !user_range_valid((uintptr_t)user_actual, sizeof(uint32_t))) {
 898 |         return AB_ERR_INVALID_ARGS;
 899 |     }
 900 | 
 901 |     ab_object_t *job_obj = NULL;
 902 |     ab_status_t st = handle_lookup(get_current_process(), job_handle, AB_OBJ_TYPE_JOB,
 903 |                                    AB_RIGHT_INSPECT, &job_obj, NULL);
 904 |     if (st != AB_OK) return st;
 905 | 
       //// KERNEL STACK PROTECTION VIA HEAP ALLOCATION:
       //// sizeof(ab_task_info_entry_t) is 152 bytes. At the maximum cap of 64
       //// entries, kentries occupies 64 * 152 = 9,728 bytes (~9.5 KB) — larger
       //// than the entire 8 KB (THREAD_STACK_SIZE) per-thread EL1 kernel stack!
       //// Declaring kentries on the stack would silently corrupt adjacent kernel
       //// memory. Allocating with kzalloc() places the buffer on the kernel heap,
       //// zero-initializes all padding bytes (preventing kernel stack/heap info
       //// leaks to EL0), and frees it unconditionally via kfree() on line 925.
 906 |     ab_task_info_entry_t *kentries =
 907 |         (ab_task_info_entry_t *)kzalloc(capped * sizeof(ab_task_info_entry_t));
 908 |     if (!kentries) {
 909 |         ab_object_release(job_obj);
 910 |         return AB_ERR_NO_MEMORY;
 911 |     }
 912 | 
 913 |     uint32_t actual = 0;
 914 |     st = ab_job_get_ps_info(job_obj, kentries, capped, &actual);
 915 |     ab_object_release(job_obj);
 916 | 
 917 |     if (st == AB_OK) {
 918 |         if (actual > 0) {
 919 |             st = copy_to_user(user_entries, kentries, actual * sizeof(ab_task_info_entry_t));
 920 |         }
 921 |         if (st == AB_OK) {
 922 |             st = copy_to_user(user_actual, &actual, sizeof(actual));
 923 |         }
 924 |     }
 925 |     kfree(kentries);
 926 |     return st;
 927 | }
 928 | 
       //// --------------------------------------------------------------------------
       //// SYSCALL 33: sys_process_get_maps (SYS_PROCESS_GET_MAPS)
       //// --------------------------------------------------------------------------
       //// Backs the 'vmaps [koid]' shell command, reporting the 3-level Zircon
       //// address-space hierarchy (VmAspace -> VMAR -> Mapping, permissions,
       //// backing VMO KOID, committed pages, and first physical address).
       ////
       //// Like sys_job_get_ps_info(), 32 * sizeof(ab_info_maps_t) = 32 * 104 =
       //// 3,328 bytes (~3.25 KB), which alongside the 288-byte SVC iframe_t and
       //// potential nested IRQ frames would dangerously pressure the 8 KB kernel
       //// stack; hence kmaps is heap-allocated with kzalloc() and freed on exit.
 929 | static ab_status_t sys_process_get_maps(ab_handle_t job_handle,
 930 |                                         const ab_process_get_maps_args_t *user_args) {
 931 |     if (!user_args) return AB_ERR_INVALID_ARGS;
 932 |     ab_process_get_maps_args_t kargs;
 933 |     ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
 934 |     if (st != AB_OK) return st;
 935 |     if (!kargs.maps_out || !kargs.actual_out || kargs.max_maps == 0) {
 936 |         return AB_ERR_INVALID_ARGS;
 937 |     }
 938 | 
 939 |     uint32_t capped = (kargs.max_maps > 32U) ? 32U : kargs.max_maps;
 940 |     if (!user_range_valid((uintptr_t)kargs.maps_out, capped * sizeof(ab_info_maps_t)) ||
 941 |         !user_range_valid((uintptr_t)kargs.actual_out, sizeof(uint32_t))) {
 942 |         return AB_ERR_INVALID_ARGS;
 943 |     }
 944 | 
 945 |     ab_object_t *job_obj = NULL;
 946 |     st = handle_lookup(get_current_process(), job_handle, AB_OBJ_TYPE_JOB,
 947 |                        AB_RIGHT_INSPECT, &job_obj, NULL);
 948 |     if (st != AB_OK) return st;
 949 | 
 950 |     ab_info_maps_t *kmaps = (ab_info_maps_t *)kzalloc(capped * sizeof(ab_info_maps_t));
 951 |     if (!kmaps) {
 952 |         ab_object_release(job_obj);
 953 |         return AB_ERR_NO_MEMORY;
 954 |     }
 955 | 
 956 |     uint32_t actual = 0;
 957 |     st = ab_job_get_process_maps(job_obj, kargs.target_proc_koid, kmaps, capped, &actual);
 958 |     ab_object_release(job_obj);
 959 | 
 960 |     if (st == AB_OK) {
 961 |         if (actual > 0) {
 962 |             st = copy_to_user(kargs.maps_out, kmaps, actual * sizeof(ab_info_maps_t));
 963 |         }
 964 |         if (st == AB_OK) {
 965 |             st = copy_to_user(kargs.actual_out, &actual, sizeof(actual));
 966 |         }
 967 |     }
 968 |     kfree(kmaps);
 969 |     return st;
 970 | }
 971 | 
```

### 11.4.3 Technical Notes

1. **Why Zircon Replaces `fork()` with `process_create` + `vmar_map` + `process_start`**: In a microkernel without ambient global authority, cloning a parent's entire address space and open descriptors (`fork()`) violates the Principle of Least Privilege—a shell spawning an untrusted utility does not want the child to inherit every mapping and capability transiently held by the shell! Instead, `sys_process_create` returns `proc_h` and `vmar_h` for a completely blank address space; the parent maps only the child's ELF/binary VMO and stack VMO via `sys_vmar_map(child_vmar_h, ...)`, creates a thread via `sys_thread_create`, and passes a single bootstrap channel handle via `sys_process_start`.
2. **W^X and Read-Only Shared Memory Protection in `sys_vmar_map`**: Lines 590–597 translate the requested `AB_VM_PERM_{READ,WRITE,EXECUTE}` mapping flags into required `AB_RIGHT_{READ,WRITE,EXECUTE}` capability bits on the VMO handle itself. Thus, after `userboot` or `fs-srv` loads an executable into a VMO, duplicating the handle without `AB_RIGHT_WRITE` freezes its contents irreversibly: no recipient process can ever map that VMO as writable or call `sys_vmo_write` on it.
3. **Microkernel User-Space Drivers Without Ambient Root**: Notice how `sys_vmo_create_physical`, `sys_vmo_create_contiguous`, and `sys_interrupt_create` all validate `resource_handle` against `AB_OBJ_TYPE_RESOURCE`. During boot (`main.c`), `lk_main` creates `root_res` and hands it only to `userboot`, which forwards it only to hardware driver servers (`console-srv` and `fs-srv`)—never to `shell` or user apps. A crash in a user-space driver simply triggers `ab_process_exit_current(-1)` rather than bringing down `EL1`.

---

## 11.5 Process Termination (ab_process_exit_current) and syscall_dispatch

### 11.5.1 Functional Description

Lines 972–1219 conclude `syscalls.c` with two foundational routines:
- `ab_process_exit_current(int exit_code)` executes complete, SMP-safe teardown of the calling user process: pinning the `ab_process_t`, evacuating `TTBR0_EL1` to `empty_ttbr0_l1`, unmapping all active VMAR mappings and releasing their backing VMOs outside `vmar_lock`, closing all remaining capability handles in slots `0..63` outside `handle_lock` so peer notifications fire cleanly, asserting `AB_SIGNAL_TASK_TERMINATED` on both the thread and process dispatchers, unlinking from the parent `Job`, releasing the process reference, and terminating the LK kernel thread via `thread_exit(exit_code)`.
- `syscall_dispatch(iframe_t *frame)` decodes `frame->r[8]` (`x8`) across all 34 `SYS_*` constants, casts `frame->r[0]..frame->r[3]` (`x0..x3`) to the exact C parameter types of each `sys_*` handler, logs unknown syscall numbers without panicking, and writes the sign-extended 64-bit `ab_status_t` result back into `frame->r[0]`.

### 11.5.2 Heavily Commented Source Code

#### Program 11-4: `lesson_20/kernel/core/syscalls.c` (Lines 972–1219)

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/syscalls.c" start="972" end="1219" total="1219" -->
```c
       //// ==========================================================================
       //// PROCESS TERMINATION & CENTRAL SYSCALL DISPATCHER (Lines 972-1219)
       //// ==========================================================================
       //// ab_process_exit_current() tears down the calling user process and never
       //// returns. It is invoked from TWO paths:
       ////   1. Voluntary exit  : EL0 calls sys_process_exit(code) (line 1072).
       ////   2. Crash isolation : arm64_sync_exception() catches an unhandled EL0
       ////                        fault (NULL deref, bad instruction, W^X violation)
       ////                        and calls ab_process_exit_current(-1) while keeping
       ////                        the kernel and all sibling servers alive!
 972 | void ab_process_exit_current(int exit_code) {
 973 |     thread_t *cur = get_current_thread();
 974 |     ab_process_t *proc = cur ? (ab_process_t *)cur->process_obj : NULL;
 975 |     ab_thread_dispatcher_t *uthr = cur ? (ab_thread_dispatcher_t *)cur->thread_obj : NULL;
 976 | 
       //// TEARDOWN STEP 1 (Self-Pinning):
       //// Increment proc->base.ref_count via ab_object_add_ref(). During teardown
       //// we will close handles (which might include a self-process handle) and
       //// unlink proc from its parent Job. Holding this extra reference guarantees
       //// proc cannot be kfree()'d out from under us before line 1042!
 977 |     if (proc) {
 978 |         ab_object_add_ref(&proc->base);
 979 |         proc->exit_code = exit_code;
 980 |         proc->exited    = 1;
 981 | 
 982 |         dprintf(INFO, "[process '%s' koid=%lu] exiting with code %d; closing remaining handles\n",
 983 |                 proc->name, proc->base.koid, exit_code);
 984 | 
       //// TEARDOWN STEP 2 (Safe TTBR0_EL1 Evacuation BEFORE Unmapping):
       //// Detach cur->aspace, cur->process_obj, and cur->thread_obj on the running
       //// LK thread_t and immediately switch TTBR0_EL1 to empty_ttbr0_l1 (ASID 0)
       //// via vmm_switch_aspace(NULL) + ISB!
       ////
       //// WHY ORDER MATTERS: Because interrupts are enabled during teardown (and
       //// the ARMv8 MMU table walker can speculatively prefetch through active
       //// TTBR0_EL1), unmapping or freeing user page tables while TTBR0_EL1 still
       //// points to proc->aspace would risk use-after-free page-table walks or
       //// corrupting context switches on preemption.
 985 |         /* Switch CPU off this process's TTBR0_EL1 before unmapping or freeing aspace */
 986 |         if (cur) {
 987 |             cur->aspace      = NULL;
 988 |             cur->process_obj = NULL;
 989 |             cur->thread_obj  = NULL;
 990 |         }
 991 |         vmm_switch_aspace(NULL);
 992 | 
       //// TEARDOWN STEP 3 (Lock-Split VMAR Unmapping & VMO Release):
       //// Under proc->vmar_lock (IRQs disabled), unmap every page of every active
       //// VMAR mapping and collect backing VMO pointers into local vmos_to_rel[].
       //// Drop vmar_lock FIRST (line 1012), THEN call ab_object_release() on each
       //// VMO (lines 1014-1016) so VMO destruction and PMM page freeing never run
       //// while holding vmar_lock.
 993 |         /* Unmap and release all active VMAR mappings */
 994 |         ab_vmo_dispatcher_t *vmos_to_rel[MAX_VMAR_MAPPINGS];
 995 |         uint32_t vmo_rel_cnt = 0;
 996 |         spin_lock_saved_state_t irqstate;
 997 |         spin_lock_irqsave(&proc->vmar_lock, &irqstate);
 998 |         for (uint32_t i = 0; i < MAX_VMAR_MAPPINGS; i++) {
 999 |             if (proc->mappings[i].active) {
1000 |                 if (proc->aspace) {
1001 |                     for (size_t off = 0; off < proc->mappings[i].size; off += PAGE_SIZE) {
1002 |                         vmm_unmap_page(proc->aspace, proc->mappings[i].base_va + off);
1003 |                     }
1004 |                 }
1005 |                 if (proc->mappings[i].vmo) {
1006 |                     vmos_to_rel[vmo_rel_cnt++] = proc->mappings[i].vmo;
1007 |                     proc->mappings[i].vmo = NULL;
1008 |                 }
1009 |                 proc->mappings[i].active = 0;
1010 |             }
1011 |         }
1012 |         spin_lock_irqrestore(&proc->vmar_lock, irqstate);
1013 | 
1014 |         for (uint32_t i = 0; i < vmo_rel_cnt; i++) {
1015 |             ab_object_release(&vmos_to_rel[i]->base);
1016 |         }
1017 | 
       //// TEARDOWN STEP 4 (Closing All Remaining Capability Handles):
       //// Scan all 64 slots of proc->handles[]. For each occupied slot, snapshot
       //// its 16-bit generation counter under handle_lock, drop handle_lock,
       //// reconstruct the public 32-bit handle value:
       ////   bits [31:28] = 0x5    (ABOS capability magic nibble)
       ////   bits [23:8]  = gen    (16-bit slot generation counter)
       ////   bits [7:0]   = slot   (table index 0..63)
       //// and invoke handle_close(proc, h). Calling handle_close() unlocked avoids
       //// recursive handle_lock deadlocks and cleanly fires on_zero_handles()
       //// (asserting AB_SIGNAL_PEER_CLOSED on any open Channel/EventPair peers so
       //// console-srv and fs-srv immediately detect client disconnection!).
1018 |         /* Close all open capability handles owned by this process */
1019 |         for (uint32_t slot = 0; slot < MAX_HANDLES_PER_PROCESS; slot++) {
1020 |             uint32_t gen = 0;
1021 |             int has_h = 0;
1022 |             spin_lock_irqsave(&proc->handle_lock, &irqstate);
1023 |             if (proc->handles[slot].obj != NULL) {
1024 |                 gen   = proc->handles[slot].generation;
1025 |                 has_h = 1;
1026 |             }
1027 |             spin_lock_irqrestore(&proc->handle_lock, irqstate);
1028 | 
1029 |             if (has_h) {
1030 |                 ab_handle_t h = (0x5U << 28) | ((gen & 0xFFFFU) << 8) | slot;
1031 |                 handle_close(proc, h);
1032 |             }
1033 |         }
1034 | 
       //// TEARDOWN STEP 5 (Task Termination Signals, Job Unlink & Thread Exit):
       ////   1. Assert AB_SIGNAL_TASK_TERMINATED on both the ThreadDispatcher and
       ////      ProcessDispatcher StateTrackers — waking any parent shell blocked
       ////      in sys_object_wait_one(child_proc_h, AB_SIGNAL_TASK_TERMINATED, ...).
       ////   2. Remove proc from its parent Job's process list.
       ////   3. Release our temporary proc reference (if no parent holds a handle,
       ////      process_destroy() now frees the user stack, page tables, ASID, and
       ////      ab_process_t; otherwise process_destroy() runs when the parent
       ////      closes its proc_h handle).
       ////   4. Call thread_exit(exit_code) to transition the LK thread to
       ////      THREAD_DEATH and context-switch away forever.
1035 |         if (uthr) {
1036 |             state_tracker_update_signals(&uthr->base, 0, AB_SIGNAL_TASK_TERMINATED);
1037 |             ab_object_release(&uthr->base);
1038 |         }
1039 | 
1040 |         state_tracker_update_signals(&proc->base, 0, AB_SIGNAL_TASK_TERMINATED);
1041 |         ab_job_remove_process(proc);
1042 |         ab_object_release(&proc->base);
1043 |     }
1044 | 
1045 |     thread_exit(exit_code);
1046 | }
1047 | 
       //// ==========================================================================
       //// CENTRAL SYSTEM CALL DISPATCHER: syscall_dispatch(iframe_t *frame)
       //// ==========================================================================
       //// Invoked from arm64_sync_exception() when an EL0 thread executes 'svc #0'
       //// (ESR_EL1.EC == 0x15 with ARM64_EXCEPTION_FLAG_LOWER_EL). Interrupts are
       //// enabled around this call so blocking syscalls can sleep and preempt.
       ////
       //// ABI Register Convention (saved in iframe_t on the thread's EL1 stack):
       ////   - frame->r[8]      (x8)     : System call number (1..34)
       ////   - frame->r[0..3]   (x0..x3) : Up to four 64-bit input arguments
       ////   - frame->r[0]      (x0)     : Signed 64-bit return status on ERET
1048 | void syscall_dispatch(iframe_t *frame) {
1049 |     uint64_t sys_num = frame->r[8];
1050 |     ab_status_t ret = AB_ERR_NOT_SUPPORTED;
1051 | 
       //// Dispatch Group A (IDs 1..6): Core Handle Operations, DebugLog,
       //// Process Exit, and Thread Nanosleep.
       //// Notice SYS_PROCESS_EXIT never reaches 'break' because
       //// ab_process_exit_current() terminates the calling thread directly.
1052 |     switch (sys_num) {
1053 |         case SYS_HANDLE_CLOSE:
1054 |             ret = sys_handle_close((ab_handle_t)frame->r[0]);
1055 |             break;
1056 |         case SYS_HANDLE_DUPLICATE:
1057 |             ret = sys_handle_duplicate((ab_handle_t)frame->r[0],
1058 |                                        (ab_rights_t)frame->r[1],
1059 |                                        (ab_handle_t *)frame->r[2]);
1060 |             break;
1061 |         case SYS_OBJECT_GET_INFO:
1062 |             ret = sys_object_get_info((ab_handle_t)frame->r[0],
1063 |                                       (ab_handle_basic_info_t *)frame->r[1]);
1064 |             break;
1065 |         case SYS_DEBUGLOG_WRITE:
1066 |             ret = sys_debuglog_write((ab_handle_t)frame->r[0],
1067 |                                      (uint32_t)frame->r[1],
1068 |                                      (const char *)frame->r[2],
1069 |                                      (size_t)frame->r[3]);
1070 |             break;
1071 |         case SYS_PROCESS_EXIT:
1072 |             ab_process_exit_current((int)frame->r[0]);
1073 |             break;
1074 |         case SYS_NANOSLEEP:
1075 |             thread_sleep_ns(frame->r[0]);
1076 |             ret = AB_OK;
1077 |             break;
       //// Dispatch Group B (IDs 7..15): Events, EventPairs, Object Signals,
       //// Synchronous Wait, and Asynchronous Event Ports.
1078 |         case SYS_EVENT_CREATE:
1079 |             ret = sys_event_create((uint32_t)frame->r[0],
1080 |                                    (ab_handle_t *)frame->r[1]);
1081 |             break;
1082 |         case SYS_EVENTPAIR_CREATE:
1083 |             ret = sys_eventpair_create((uint32_t)frame->r[0],
1084 |                                        (ab_handle_t *)frame->r[1],
1085 |                                        (ab_handle_t *)frame->r[2]);
1086 |             break;
1087 |         case SYS_OBJECT_SIGNAL:
1088 |             ret = sys_object_signal((ab_handle_t)frame->r[0],
1089 |                                     (ab_signals_t)frame->r[1],
1090 |                                     (ab_signals_t)frame->r[2]);
1091 |             break;
1092 |         case SYS_OBJECT_SIGNAL_PEER:
1093 |             ret = sys_object_signal_peer((ab_handle_t)frame->r[0],
1094 |                                          (ab_signals_t)frame->r[1],
1095 |                                          (ab_signals_t)frame->r[2]);
1096 |             break;
1097 |         case SYS_OBJECT_WAIT_ONE:
1098 |             ret = sys_object_wait_one((ab_handle_t)frame->r[0],
1099 |                                       (ab_signals_t)frame->r[1],
1100 |                                       (uint64_t)frame->r[2],
1101 |                                       (ab_signals_t *)frame->r[3]);
1102 |             break;
1103 |         case SYS_PORT_CREATE:
1104 |             ret = sys_port_create((uint32_t)frame->r[0],
1105 |                                   (ab_handle_t *)frame->r[1]);
1106 |             break;
1107 |         case SYS_PORT_QUEUE:
1108 |             ret = sys_port_queue((ab_handle_t)frame->r[0],
1109 |                                  (const ab_port_packet_t *)frame->r[1]);
1110 |             break;
1111 |         case SYS_PORT_WAIT:
1112 |             ret = sys_port_wait((ab_handle_t)frame->r[0],
1113 |                                 (uint64_t)frame->r[1],
1114 |                                 (ab_port_packet_t *)frame->r[2]);
1115 |             break;
1116 |         case SYS_OBJECT_WAIT_ASYNC:
1117 |             ret = sys_object_wait_async((ab_handle_t)frame->r[0],
1118 |                                         (ab_handle_t)frame->r[1],
1119 |                                         (uint64_t)frame->r[2],
1120 |                                         (ab_signals_t)frame->r[3]);
1121 |             break;
       //// Dispatch Group C (IDs 16..22, plus 34 below): Channels (IPC + Handle
       //// Transfer), Virtual Memory Objects (VMO), and Address Regions (VMAR).
1122 |         case SYS_CHANNEL_CREATE:
1123 |             ret = sys_channel_create((uint32_t)frame->r[0],
1124 |                                      (ab_handle_t *)frame->r[1],
1125 |                                      (ab_handle_t *)frame->r[2]);
1126 |             break;
1127 |         case SYS_CHANNEL_WRITE:
1128 |             ret = sys_channel_write((ab_handle_t)frame->r[0],
1129 |                                     (uint32_t)frame->r[1],
1130 |                                     (const ab_channel_write_args_t *)frame->r[2]);
1131 |             break;
1132 |         case SYS_CHANNEL_READ:
1133 |             ret = sys_channel_read((ab_handle_t)frame->r[0],
1134 |                                    (uint32_t)frame->r[1],
1135 |                                    (ab_channel_read_args_t *)frame->r[2]);
1136 |             break;
1137 |         case SYS_VMO_CREATE:
1138 |             ret = sys_vmo_create((uint64_t)frame->r[0],
1139 |                                  (uint32_t)frame->r[1],
1140 |                                  (ab_handle_t *)frame->r[2]);
1141 |             break;
1142 |         case SYS_VMO_READ:
1143 |             ret = sys_vmo_read((ab_handle_t)frame->r[0],
1144 |                                (void *)frame->r[1],
1145 |                                (uint64_t)frame->r[2],
1146 |                                (size_t)frame->r[3]);
1147 |             break;
1148 |         case SYS_VMO_WRITE:
1149 |             ret = sys_vmo_write((ab_handle_t)frame->r[0],
1150 |                                 (const void *)frame->r[1],
1151 |                                 (uint64_t)frame->r[2],
1152 |                                 (size_t)frame->r[3]);
1153 |             break;
1154 |         case SYS_VMAR_MAP:
1155 |             ret = sys_vmar_map((ab_handle_t)frame->r[0],
1156 |                                (size_t)frame->r[1],
1157 |                                (const ab_vmar_map_args_t *)frame->r[2]);
1158 |             break;
1159 |         case SYS_VMAR_UNMAP:
1160 |             ret = sys_vmar_unmap((ab_handle_t)frame->r[0],
1161 |                                  (vaddr_t)frame->r[1],
1162 |                                  (size_t)frame->r[2]);
1163 |             break;
       //// Dispatch Group D (IDs 23..30): Zircon Task Lifecycle (Process/Thread
       //// Create & Start) and User-Space Device Driver Kit (Physical/Contiguous
       //// DMA VMOs & Hardware Interrupt Objects).
1164 |         case SYS_PROCESS_CREATE:
1165 |             ret = sys_process_create((ab_handle_t)frame->r[0],
1166 |                                      (const ab_process_create_args_t *)frame->r[1]);
1167 |             break;
1168 |         case SYS_THREAD_CREATE:
1169 |             ret = sys_thread_create((ab_handle_t)frame->r[0],
1170 |                                     (const char *)frame->r[1],
1171 |                                     (size_t)frame->r[2],
1172 |                                     (ab_handle_t *)frame->r[3]);
1173 |             break;
1174 |         case SYS_PROCESS_START:
1175 |             ret = sys_process_start((ab_handle_t)frame->r[0],
1176 |                                     (const ab_process_start_args_t *)frame->r[1]);
1177 |             break;
1178 |         case SYS_VMO_CREATE_PHYSICAL:
1179 |             ret = sys_vmo_create_physical((ab_handle_t)frame->r[0],
1180 |                                           (const ab_vmo_create_phys_args_t *)frame->r[1]);
1181 |             break;
1182 |         case SYS_VMO_CREATE_CONTIG:
1183 |             ret = sys_vmo_create_contiguous((ab_handle_t)frame->r[0],
1184 |                                             (const ab_vmo_create_contig_args_t *)frame->r[1]);
1185 |             break;
1186 |         case SYS_INTERRUPT_CREATE:
1187 |             ret = sys_interrupt_create((ab_handle_t)frame->r[0],
1188 |                                        (uint32_t)frame->r[1],
1189 |                                        (ab_handle_t *)frame->r[2]);
1190 |             break;
1191 |         case SYS_INTERRUPT_WAIT:
1192 |             ret = sys_interrupt_wait((ab_handle_t)frame->r[0]);
1193 |             break;
1194 |         case SYS_INTERRUPT_ACK:
1195 |             ret = sys_interrupt_ack((ab_handle_t)frame->r[0]);
1196 |             break;
       //// Dispatch Group E (IDs 31..33 & default): Object Diagnostic Naming,
       //// Job/Process Introspection ('ps' and 'vmaps'), and Unknown Syscall
       //// Fallback (logs CRITICAL diagnostic and returns AB_ERR_NOT_SUPPORTED,
       //// never crashing the kernel on an invalid x8 value).
1197 |         case SYS_OBJECT_SET_NAME:
1198 |             ret = sys_object_set_name((ab_handle_t)frame->r[0],
1199 |                                       (const char *)frame->r[1],
1200 |                                       (size_t)frame->r[2]);
1201 |             break;
1202 |         case SYS_JOB_GET_PS_INFO:
1203 |             ret = sys_job_get_ps_info((ab_handle_t)frame->r[0],
1204 |                                       (ab_task_info_entry_t *)frame->r[1],
1205 |                                       (uint32_t)frame->r[2],
1206 |                                       (uint32_t *)frame->r[3]);
1207 |             break;
1208 |         case SYS_PROCESS_GET_MAPS:
1209 |             ret = sys_process_get_maps((ab_handle_t)frame->r[0],
1210 |                                        (const ab_process_get_maps_args_t *)frame->r[1]);
1211 |             break;
1212 |         default:
1213 |             dprintf(CRITICAL, "Unknown syscall %lu\n", sys_num);
1214 |             ret = AB_ERR_NOT_SUPPORTED;
1215 |             break;
1216 |     }
1217 | 
       //// Write the 32-bit signed ab_status_t return code into frame->r[0] (x0),
       //// sign-extending via (int64_t) first so negative AB_ERR_* codes remain
       //// clean 64-bit two's-complement values when exceptions.S restores x0..x30
       //// and executes 'eret' back to EL0.
1218 |     frame->r[0] = (uint64_t)(int64_t)ret;
1219 | }
```

### 11.5.3 Technical Notes

1. **Unified Voluntary Exit and Crash Isolation**: Compare `SYS_PROCESS_EXIT` on line 1072 with `arm64_sync_exception` in `exceptions_c.c` (line 59): both funnel into `ab_process_exit_current()`. Consequently, even if a standalone server or user app dereferences `NULL` or executes an illegal instruction, every single VMAR mapping, VMO reference, ASID, user stack page, and open IPC channel handle is reclaimed identically to a clean `exit(0)`—and any client or shell waiting on its channel or process handle immediately wakes up via `AB_SIGNAL_PEER_CLOSED` / `AB_SIGNAL_TASK_TERMINATED` instead of hanging forever!
2. **Two-Phase Process Reclamation (`ab_process_exit_current` vs `process_destroy`)**: Why does `ab_process_exit_current` unmap VMARs and close handles immediately, whereas `process_destroy` in `object.c` frees the `EL0` stack pages and `vm_aspace_t` later? Because a parent `shell` typically still holds an open `proc_h` handle so it can call `sys_object_wait_one(proc_h, AB_SIGNAL_TASK_TERMINATED, ...)`! Reclaiming VMARs and open handles inside `ab_process_exit_current` releases bulk memory and notifies IPC peers immediately at exit time, while keeping the lightweight `ab_process_t` shell alive until the parent closes `proc_h` (dropping `ref_count` to zero and triggering `process_destroy`).
3. **64-Bit Sign Extension of `ab_status_t` on Return**: On AArch64, `ab_status_t` is `int32_t` (for example, `AB_ERR_INVALID_ARGS = -10 = 0xFFFFFFF6`). Casting `(uint64_t)(int64_t)ret` on line 1218 sign-extends `-10` to `0xFFFFFFFFFFFFFFF6` in `frame->r[0]`, ensuring both 32-bit (`w0`) and 64-bit (`x0`) inspection in `EL0` C/assembly callers observe exact negative status values.

---

## 11.6 Summary

`kernel/core/syscalls.c` embodies the complete security perimeter of the ABOS microkernel:
- **Zero Ambient Authority**: None of the 34 system calls accept file paths, PIDs for mutation, or raw hardware addresses without first validating a per-process capability handle (`ab_handle_t`) and its `ab_rights_t` bitmask via `handle_lookup()`.
- **Zero Raw User Dereferences**: Every byte crossing between `EL0` and `EL1` passes through `user_range_valid()`, software page-table translation + demand-faulting in `resolve_user_page()`, and `TTBR1_EL1` `physmap` copies in `copy_from_user()` / `copy_to_user()`.
- **Bounded Stack & Clean Rollback**: Multi-argument descriptors are copied once onto the stack to defeat TOCTOU races, multi-kilobyte `ps`/`vmaps` tables use `kzalloc()`/`kfree()` to protect the 8 KB `EL1` stack, failed `copy_to_user()` exports roll back newly created handles/mappings, and `ab_process_exit_current()` evacuates `TTBR0_EL1` before tearing down address spaces and capabilities.

In Chapter 12, we examine all 14 header files in `lesson_20/include/` (`1,213` lines total), which define the hardware register layouts, LK scheduling primitives, Zircon object dispatchers, system call numbers, and user-space IPC wire protocols underpinning the entire system.
