# 12 Kernel and Shared Header Files (include/)

Following the tradition of Chapter 14 (*Header Files*) in Dr. Zhao Jiong's *A Heavily Commented Linux Kernel Source Code*, this chapter presents and annotates every single line of all **14 header files** (`1,213` lines in total) inside [`lesson_20/include/`](../../lesson_20/include/). Studying these headers together reveals the complete type skeleton of the ABOS microkernel—from single-instruction AArch64 `DAIF` and `LDAXR`/`STLR` spinlock primitives up to Zircon `Dispatcher` inheritance, capability rights, system call numbers, and user-space server wire protocols.

---

## 12.1 Overview of Files in include/

In ABOS Lesson 20, both the `EL1` kernel (`kernel/`) and the `EL0` user-space tree (`user/ulibc/`, `drivers/`, `userboot/`, `shell/`, `apps/`) compile against the shared `lesson_20/include/` directory (`-Iinclude`). Just as Linux 0.11 shared declarations across `include/linux/` and `include/sys/` while separating hardware mechanisms from user contracts, ABOS organizes its 14 headers into **four strictly acyclic architectural layers** (a Directed Acyclic Graph with zero circular `#include` dependencies):

```text
  ================================================================================
  LAYER 3: ZIRCON CAPABILITY OBJECTS, SYSCALL ABI & USER-SPACE IPC CONTRACTS
    [Shared across EL1 Kernel & EL0 ulibc]
      ipc_proto.h (58L) ---> syscalls.h (53L) ---> object.h (590L)
  ================================================================================
                                    |                   |
                                    | (exceptions.h)    | (spinlock, mmu, thread, wait)
                                    v                   v
  ================================================================================
  LAYER 2: LK PREEMPTIVE SCHEDULER & BLOCKING SYNCHRONIZATION PRIMITIVES
      wait.h (43L) -------> thread.h (90L)
  ================================================================================
                                    |
                                    | (mmu.h, timer.h)
                                    v
  ================================================================================
  LAYER 1: VIRTUAL/PHYSICAL MEMORY MANAGEMENT & INTERRUPT CONTROLLER
      gic.h (32L) --------> mmu.h (84L) --------> pmm.h (48L)      heap.h (19L)
  ================================================================================
                                    |
                                    | (spinlock.h)
                                    v
  ================================================================================
  LAYER 0: AARCH64 HARDWARE, BOOT DTB & CONSOLE PRIMITIVES (ZERO LOCAL INCLUDES)
      spinlock.h (83L)   debuglog.h (23L)   dtb.h (35L)   exceptions.h (34L)   timer.h (21L)
  ================================================================================
```

Notice how forward struct declarations (`struct wait_queue` in `thread.h`; `struct ab_object`, `struct ab_thread_dispatcher`, and `struct ab_job_dispatcher` in `object.h`) keep the `#include` graph 100% cycle-free while preserving strong static typing across layers. While `EL0` translation units parse `object.h`, `syscalls.h`, and `ipc_proto.h` via `ulibc.h` to obtain shared ABI constants (`SYS_*`, `AB_RIGHT_*`, `AB_SIGNAL_*`, `AB_ERR_*`) and argument structs, `EL0` code invokes *only* the `SVC #0` wrappers in `ulibc`—any attempt by `EL0` to execute privileged instructions such as `mrs mpidr_el1` or `msr daifset` would immediately trap as an unhandled `EL0` exception and terminate the offending process.

### Master Dependency and Layer Table of `lesson_20/include/`

| Sec | Header File | Lines | Layer | Direct Local `#include`s | Scope | Primary Types, Constants & Subsystem Contracts |
|---|---|---:|---|---|---|---|
| 12.2 | `include/spinlock.h` | 83 | L0: Arch/SMP | *(none)* | EL1 | `spin_lock_t`, `arch_spin_lock` (`sevl`/`wfe`/`ldaxr`/`stxr`), `DAIF` IRQ save/restore |
| 12.3 | `include/debuglog.h` | 23 | L0: Console | *(none)* | EL1 | `CRITICAL`/`INFO`/`SPEW`, PL011 UART output, `dprintf` |
| 12.4 | `include/dtb.h` | 35 | L0: Boot/FDT | *(none)* | EL1 | `FDT_MAGIC`, `struct fdt_header`, `boot_info_t`, `dtb_parse` |
| 12.5 | `include/exceptions.h` | 34 | L0: Arch/Trap | *(none)* | EL1 | 288-byte `iframe_t`, `ARM64_EXCEPTION_FLAG_LOWER_EL`, sync/IRQ entry points |
| 12.6 | `include/gic.h` | 32 | L1: IRQ Ctrl | `mmu.h` | EL1 | Higher-half `GICD_*` & `GICC_*` MMIO registers, GICv2 init/ack/EOI |
| 12.7 | `include/timer.h` | 21 | L0: Arch/Time | *(none)* | EL1 | `TIMER_ARCH_PHYS_IRQ` (30), `ab_time_t`, `current_time_ns` |
| 12.8 | `include/pmm.h` | 48 | L1: Phys Mem | *(none)* | EL1 | `PAGE_SIZE` (4 KB), `vm_page_state_t`, 16-byte `vm_page_t`, PMM alloc/free |
| 12.9 | `include/heap.h` | 19 | L1: Kernel Heap | *(none)* | EL1 | `heap_init`, `kmalloc`, `kzalloc`, `kfree`, `heap_dump_stats` |
| 12.10 | `include/mmu.h` | 84 | L1: Virt Mem | `pmm.h`, `spinlock.h` | EL1 | `KERNEL_PHYSMAP_OFFSET`, Stage-1 `PTE_*` bits, `USER_ASPACE_*`, `vm_aspace_t` |
| 12.11 | `include/thread.h` | 90 | L2: Scheduler | `mmu.h`, `timer.h` | EL1 | 8 KB stacks, `context_switch_frame_t`, `thread_t` TCB, SMP scheduler API |
| 12.12 | `include/wait.h` | 43 | L2: Blocking | `thread.h` | EL1 | `wait_queue_t`, `event_t`, `EVENT_FLAG_AUTOUNSIGNAL`, timed wait/wake |
| 12.13 | `include/object.h` | 590 | L3: Objects | `spinlock.h`, `mmu.h`, `thread.h`, `wait.h` | EL1 + EL0 | `ab_object_t`, rights, signals, `StateTracker`, all 12 dispatchers, `ps`/`vmaps` ABI |
| 12.14 | `include/syscalls.h` | 53 | L3: Syscalls | `object.h`, `exceptions.h` | EL1 + EL0 | `SYS_HANDLE_CLOSE` (1) .. `SYS_VMAR_UNMAP` (34), `syscall_dispatch`, user copy |
| 12.15 | `include/ipc_proto.h` | 58 | L3: User IPC | `object.h`, `syscalls.h` | EL1 + EL0 | `AB_PA_*` bootstrap tags, `ab_proc_args_t`, `console_msg_t`, `fs_msg_t` |
| **Total** | **14 Header Files** | **1,213** | **L0 -> L3** | **Strict Acyclic DAG** | | **100% of ABOS Kernel & Shared ABI Declarations** |

---

## 12.2 include/spinlock.h

### 12.2.1 Functional Description

`include/spinlock.h` (83 lines) is the lowest-level concurrency header in ABOS. Zero-dependency apart from `<stdint.h>`, it provides inline assembly helpers to query the current CPU core (`MPIDR_EL1`) and exception level (`CurrentEL`), mask/unmask/save/restore local `PSTATE.I` interrupts via `DAIF`, acquire/release exclusive monitor spinlocks (`LDAXR`/`STXR`/`STLR` with `SEVL`/`WFE` power-saving backoff), and combine IRQ masking with spinlock acquisition (`spin_lock_irqsave` / `spin_lock_irqrestore`).

### 12.2.2 Heavily Commented Source Code

#### Program 12-1: `lesson_20/include/spinlock.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/spinlock.h" start="1" end="83" total="83" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/spinlock.h (83 lines)
       //// ==========================================================================
       //// Defines AArch64 SMP spinlocks, CPU/Exception-Level identification, and
       //// local IRQ save/restore primitives modeled directly after Zircon's
       //// arch/arm64/spinlock.cpp and arch_ops.h.
   1 | /* =========================================================================
   2 |  * spinlock.h - Zircon-style AArch64 Spinlock & Interrupt Save/Restore
   3 |  * References:
   4 |  *   - zircon_org/kernel/arch/arm64/spinlock.cpp
   5 |  *   - zircon_org/kernel/arch/arm64/include/arch/arch_ops.h
   6 |  * ========================================================================= */
   7 | #ifndef ABOS_SPINLOCK_H
   8 | #define ABOS_SPINLOCK_H
   9 | 
  10 | #include <stdint.h>
  11 | 
       //// QEMU virt SMP topology cap (4 Cortex-A53/A72 cores) and lock types:
       ////   - spin_lock_t.value == 0      : Lock is unheld.
       ////   - spin_lock_t.value == cpu+1  : Held by CPU 'cpu' (1..4), making lock
       ////     ownership immediately visible in GDB or panic dumps!
       ////   - spin_lock_saved_state_t     : Raw 64-bit snapshot of DAIF.
  12 | #define SMP_MAX_CPUS 4
  13 | 
  14 | typedef struct {
  15 |     volatile uint64_t value;
  16 | } spin_lock_t;
  17 | 
  18 | typedef uint64_t spin_lock_saved_state_t;
  19 | 
  20 | #define SPIN_LOCK_INITIAL_VALUE { 0 }
  21 | 
       //// arch_curr_cpu_num() reads MPIDR_EL1 (Multiprocessor Affinity Register)
       //// and extracts Aff0 (bits [7:0]), yielding 0..3 on QEMU virt.
  22 | static inline uint32_t arch_curr_cpu_num(void) {
  23 |     uint64_t mpidr;
  24 |     __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  25 |     return (uint32_t)(mpidr & 0xFF);
  26 | }
  27 | 
       //// arch_boot_get_el() reads CurrentEL; bits [3:2] encode the current
       //// exception level (1 = EL1 kernel mode, 2 = EL2 hypervisor at boot).
  28 | static inline uint32_t arch_boot_get_el(void) {
  29 |     uint64_t el;
  30 |     __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
  31 |     return (uint32_t)((el >> 2) & 0x3);
  32 | }
  33 | 
       //// Fast PSTATE interrupt toggling via immediate MSR to DAIFClr / DAIFSet:
       //// In the 4-bit immediate encoding of DAIFSet/DAIFClr, bit 0 = F (FIQ),
       //// bit 1 = I (IRQ), bit 2 = A (SError), bit 3 = D (Debug). Thus #2 clears
       //// or sets PSTATE.I (IRQ mask) in a single cycle, with a compiler 'memory'
       //// clobber preventing load/store reordering across the IRQ boundary.
  34 | static inline void arch_enable_ints(void) {
  35 |     __asm__ volatile("msr daifclr, #2" ::: "memory");
  36 | }
  37 | 
  38 | static inline void arch_disable_ints(void) {
  39 |     __asm__ volatile("msr daifset, #2" ::: "memory");
  40 | }
  41 | 
       //// Nested-safe IRQ masking: arch_interrupt_save() snapshots the entire
       //// DAIF register BEFORE setting PSTATE.I = 1, and arch_interrupt_restore()
       //// writes the saved DAIF back verbatim. If IRQs were already disabled by
       //// an outer critical section, spin_lock_irqrestore() leaves them disabled!
  42 | static inline void arch_interrupt_save(spin_lock_saved_state_t *statep) {
  43 |     uint64_t state;
  44 |     __asm__ volatile("mrs %0, daif" : "=r"(state));
  45 |     __asm__ volatile("msr daifset, #2" ::: "memory");
  46 |     *statep = state;
  47 | }
  48 | 
  49 | static inline void arch_interrupt_restore(spin_lock_saved_state_t old_state) {
  50 |     __asm__ volatile("msr daif, %0" :: "r"(old_state) : "memory");
  51 | }
  52 | 
       //// ARMv8 Load-Acquire Exclusive / Store-Exclusive WFE spinlock:
       ////   1. sevl          : Set local Event Register so the first 'wfe' falls
       ////                      straight through without sleeping.
       ////   2. 1: wfe        : On contention, put core into low-power standby until
       ////                      the exclusive monitor clears on cache-line write!
       ////   3. ldaxr         : Load-Acquire Exclusive (&lock->value) arms the
       ////                      local exclusive monitor and orders subsequent loads.
       ////   4. cbnz temp, 1b : If non-zero (already held), loop back to wfe.
       ////   5. stxr          : Attempt atomic store of (cpu_num + 1); returns 0 in
       ////                      %w[temp] on success or 1 if another CPU raced.
       ////   6. cbnz temp, 1b : Retry on exclusive store failure.
       //// Notice the '=&r' earlyclobber constraint on [temp] so GCC/Clang never
       //// assigns [temp] to the same register as [lock] or [val]!
  53 | static inline void arch_spin_lock(spin_lock_t *lock) {
  54 |     uint64_t val = (uint64_t)arch_curr_cpu_num() + 1;
  55 |     uint64_t temp;
  56 | 
  57 |     __asm__ volatile(
  58 |         "sevl\n"
  59 |         "1: wfe\n"
  60 |         "ldaxr %[temp], [%[lock]]\n"
  61 |         "cbnz  %[temp], 1b\n"
  62 |         "stxr  %w[temp], %[val], [%[lock]]\n"
  63 |         "cbnz  %w[temp], 1b\n"
  64 |         : [temp] "=&r"(temp)
  65 |         : [lock] "r"(&lock->value), [val] "r"(val)
  66 |         : "cc", "memory");
  67 | }
  68 | 
       //// arch_spin_unlock() executes a single Store-Release of XZR (0) to
       //// &lock->value. Under the ARMv8 memory model, clearing a monitored
       //// exclusive granule via STLR automatically generates a wakeup event to
       //// peer cores waiting in WFE — no explicit 'sev' instruction is required!
  69 | static inline void arch_spin_unlock(spin_lock_t *lock) {
  70 |     __asm__ volatile("stlr xzr, [%0]" :: "r"(&lock->value) : "memory");
  71 | }
  72 | 
       //// Combined IRQ-safe spinlock wrappers:
       ////   - Acquire order : disable local IRQs FIRST, THEN spin on lock (if we
       ////     took the lock with IRQs enabled and a timer ISR on the SAME CPU
       ////     tried to acquire the same lock, the CPU would deadlock forever!).
       ////   - Release order : release lock FIRST, THEN restore previous DAIF.
  73 | static inline void spin_lock_irqsave(spin_lock_t *lock, spin_lock_saved_state_t *statep) {
  74 |     arch_interrupt_save(statep);
  75 |     arch_spin_lock(lock);
  76 | }
  77 | 
  78 | static inline void spin_lock_irqrestore(spin_lock_t *lock, spin_lock_saved_state_t old_state) {
  79 |     arch_spin_unlock(lock);
  80 |     arch_interrupt_restore(old_state);
  81 | }
  82 | 
  83 | #endif /* ABOS_SPINLOCK_H */
```
### 12.2.3 Technical Notes

1. **Why `SEVL` + `WFE` + `STLR` Eliminates Explicit `SEV`**: On ARMv8-A, executing `STLR` (Store-Release) in `arch_spin_unlock()` clears the global exclusive monitor for `&lock->value`, which architecturally sets the hardware Event Register on every CPU monitoring that cache line and wakes cores sleeping in `WFE` without broadcasting an expensive system-wide `SEV` across uninvolved CPUs. Prepending `SEVL` (Set Event Local) before the loop sets only the calling CPU's own Event Register so its very first `WFE` consumes that local event immediately without stalling when the lock is free.
2. **Diagnostic Value of Storing `cpu_num + 1`**: Because CPU numbers start at `0`, storing `arch_curr_cpu_num() + 1` (`1..4`) distinguishes an unheld lock (`0`) from a lock held by CPU 0 (`1`), allowing instant deadlock attribution when inspecting `spin_lock_t` fields in QEMU/GDB.

---

## 12.3 include/debuglog.h

### 12.3.1 Functional Description

`include/debuglog.h` (23 lines) exposes the kernel's early serial console interface over QEMU `virt`'s ARM PrimeCell PL011 UART, along with LK/Zircon-compatible log severity levels (`CRITICAL`, `ALWAYS`, `INFO`, `SPEW`) and the variadic `dprintf()` formatter.

### 12.3.2 Heavily Commented Source Code

#### Program 12-2: `lesson_20/include/debuglog.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/debuglog.h" start="1" end="23" total="23" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/debuglog.h (23 lines)
       //// ==========================================================================
       //// Declares the early PL011 UART console driver and LK/Zircon-style
       //// severity-filtered kernel printf (dprintf).
   1 | /* =========================================================================
   2 |  * debuglog.h - Early Kernel Debug Console (PL011 UART + Zircon-style dprintf)
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_DEBUGLOG_H
   5 | #define ABOS_DEBUGLOG_H
   6 | 
   7 | #include <stdint.h>
   8 | 
       //// Log verbosity thresholds modeled after LK/Zircon's debug.h:
       ////   - CRITICAL / ALWAYS (0) : Panics, unhandled faults, boot banners.
       ////   - INFO              (1) : Normal subsystem init & debuglog syscalls.
       ////   - SPEW              (2) : High-frequency page-fault / object traces.
       //// KERNEL_LOG_LEVEL defaults to INFO and can be overridden via -D in CFLAGS.
   9 | #define CRITICAL 0
  10 | #define ALWAYS   0
  11 | #define INFO     1
  12 | #define SPEW     2
  13 | 
  14 | #ifndef KERNEL_LOG_LEVEL
  15 | #define KERNEL_LOG_LEVEL INFO
  16 | #endif
  17 | 
       //// Console output API: dlog_init_early() configures PL011 registers via
       //// KERNEL_PERIPHMAP_OFFSET; uart_putc()/uart_puts() emit raw characters;
       //// and dprintf() serializes multi-core formatted output under a spinlock.
  18 | void dlog_init_early(void);
  19 | void uart_putc(char c);
  20 | void uart_puts(const char *s);
  21 | void dprintf(int level, const char *fmt, ...);
  22 | 
  23 | #endif /* ABOS_DEBUGLOG_H */
```
### 12.3.3 Technical Notes

1. **Two-Tier Console Architecture**: During kernel bring-up and inside `PANIC` handlers, `EL1` writes directly through `dprintf()` to the higher-half periphmap PL011 alias (`0xFFFFFFFFC9000000`). Once `EL0` servers start, unprivileged processes emit diagnostics either through capability-gated `SYS_DEBUGLOG_WRITE` (which formats `[debuglog cpu=... proc=...]` via `dprintf`) or through `Channel` IPC to the user-space `console-srv` driver managing four virtual terminals.

---

## 12.4 include/dtb.h

### 12.4.1 Functional Description

`include/dtb.h` (35 lines) declares the standard Flattened Device Tree (`struct fdt_header`) wire layout and the `boot_info_t` summary populated by `dtb_parse()` during early boot from the physical DTB address passed in `x0` by QEMU's bootloader.

### 12.4.2 Heavily Commented Source Code

#### Program 12-3: `lesson_20/include/dtb.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/dtb.h" start="1" end="35" total="35" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/dtb.h (35 lines)
       //// ==========================================================================
       //// Defines the Flattened Device Tree (FDT) binary header and the parsed
       //// boot_info_t handoff structure populated from QEMU's x0 DTB pointer.
   1 | /* =========================================================================
   2 |  * dtb.h - Flattened Device Tree (FDT) / Boot Handoff Parser
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_DTB_H
   5 | #define ABOS_DTB_H
   6 | 
   7 | #include <stdint.h>
   8 | 
       //// Big-endian magic number 0xd00dfeed mandated by the Devicetree
       //// Specification at byte offset 0 of every valid .dtb blob.
   9 | #define FDT_MAGIC 0xd00dfeedU
  10 | 
       //// 40-byte on-wire FDT header (all ten 32-bit fields are stored in
       //// big-endian network byte order in memory and byte-swapped by dtb.c):
       ////   - totalsize      : Total byte length of the DTB image (reserved in PMM).
       ////   - off_dt_struct  : Byte offset to token stream (/memory, /chosen).
       ////   - off_dt_strings : Byte offset to property-name string table.
       ////   - off_mem_rsvmap : Byte offset to reserved physical memory table.
  11 | struct fdt_header {
  12 |     uint32_t magic;
  13 |     uint32_t totalsize;
  14 |     uint32_t off_dt_struct;
  15 |     uint32_t off_dt_strings;
  16 |     uint32_t off_mem_rsvmap;
  17 |     uint32_t version;
  18 |     uint32_t last_comp_version;
  19 |     uint32_t boot_cpuid_phys;
  20 |     uint32_t size_dt_strings;
  21 |     uint32_t size_dt_struct;
  22 | };
  23 | 
       //// Normalized host-endian hardware discovery summary consumed by pmm_init()
       //// and lk_main(): captures main RAM [ram_base, ram_base + ram_size), the
       //// DTB's own physical footprint, and the linux,initrd-start / initrd-end
       //// range carrying ABOS's bootfs.tar archive!
  24 | typedef struct {
  25 |     uint64_t ram_base;
  26 |     uint64_t ram_size;
  27 |     uint64_t dtb_paddr;
  28 |     uint32_t dtb_size;
  29 |     uint64_t initrd_start;
  30 |     uint64_t initrd_end;
  31 | } boot_info_t;
  32 | 
  33 | int dtb_parse(uint64_t dtb_paddr, boot_info_t *out_info);
  34 | 
  35 | #endif /* ABOS_DTB_H */
```
### 12.4.3 Technical Notes

1. **Zero Hardcoded RAM or `initrd` Addresses**: By parsing `/memory@40000000` (`reg`) and `/chosen` (`linux,initrd-start` and `linux,initrd-end`) dynamically into `boot_info_t`, ABOS adapts automatically if QEMU is launched with `-m 128M` vs `-m 64M` or if `bootfs.tar` grows as new user-space binaries are added.

---

## 12.5 include/exceptions.h

### 12.5.1 Functional Description

`include/exceptions.h` (34 lines) defines `iframe_t` (`struct arm64_iframe`), the 288-byte hardware register snapshot pushed onto the current thread's `EL1` stack on every synchronous exception (`SVC #0`, Data/Instruction Abort) or asynchronous hardware IRQ from `EL0` or `EL1`.

### 12.5.2 Heavily Commented Source Code

#### Program 12-4: `lesson_20/include/exceptions.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/exceptions.h" start="1" end="34" total="34" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/exceptions.h (34 lines)
       //// ==========================================================================
       //// Defines the 288-byte AArch64 hardware trap frame (iframe_t) shared
       //// between assembly vector stubs in exceptions.S, C trap handlers in
       //// exceptions_c.c, and syscall_dispatch() in syscalls.c.
   1 | /* =========================================================================
   2 |  * exceptions.h - Zircon-Style AArch64 Exception Frame (iframe_t)
   3 |  * References:
   4 |  *   - zircon_org/kernel/arch/arm64/include/arch/arm64.h
   5 |  *   - zircon_org/kernel/arch/arm64/include/arch/arm64/exceptions.h
   6 |  * ========================================================================= */
   7 | #ifndef ABOS_EXCEPTIONS_H
   8 | #define ABOS_EXCEPTIONS_H
   9 | 
  10 | #include <stdint.h>
  11 | 
       //// Bit flag passed by exceptions.S to distinguish traps originating from
       //// EL0 user space (ARM64_EXCEPTION_FLAG_LOWER_EL = 1) vs EL1 kernel mode (0).
  12 | #define ARM64_EXCEPTION_FLAG_LOWER_EL 0x1
  13 | 
  14 | /*
  15 |  * Matches Zircon's arm64_iframe_long (288 bytes = 18 * 16B aligned).
  16 |  * Pushed onto the current CPU/thread kernel stack on synchronous exceptions
  17 |  * and IRQs from either EL1 or EL0.
  18 |  */
       //// Exact binary layout of iframe_t (288 bytes = 18 * 16 bytes, preserving
       //// the hardware-mandated 16-byte alignment of SP_EL1):
       ////   +---------+------------------+--------------------------------------+
       ////   | Offset  | Field            | Hardware Register / Role             |
       ////   +---------+------------------+--------------------------------------+
       ////   |   0..232| r[0] .. r[29]    | General regs x0..x29 (x8 = syscall#) |
       ////   |  240    | lr               | Link register x30                    |
       ////   |  248    | usp              | User stack pointer SP_EL0            |
       ////   |  256    | elr              | Exception return PC ELR_EL1          |
       ////   |  264    | spsr             | Saved processor state SPSR_EL1       |
       ////   |  272    | mdscr            | Debug control register MDSCR_EL1     |
       ////   |  280    | pad              | 8-byte alignment pad -> 288B total   |
       ////   +---------+------------------+--------------------------------------+
       //// Saving SP_EL0, ELR_EL1, and SPSR_EL1 inside the per-thread stack frame
       //// allows blocking syscalls and preempted threads to context-switch safely!
  19 | typedef struct arm64_iframe {
  20 |     uint64_t r[30];     /* x0 .. x29 (offset 0..232) */
  21 |     uint64_t lr;        /* x30       (offset 240) */
  22 |     uint64_t usp;       /* sp_el0    (offset 248) */
  23 |     uint64_t elr;       /* elr_el1   (offset 256) */
  24 |     uint64_t spsr;      /* spsr_el1  (offset 264) */
  25 |     uint64_t mdscr;     /* mdscr_el1 (offset 272) */
  26 |     uint64_t pad;       /* 16-byte stack alignment padding (offset 280) */
  27 | } iframe_t;
  28 | 
       //// Vector table base symbol installed into VBAR_EL1 at boot and C entry
       //// points for synchronous traps (SVC, page faults) and GIC hardware IRQs.
  29 | extern void arm64_el1_exception_base(void);
  30 | void dump_iframe(const iframe_t *iframe);
  31 | void arm64_sync_exception(iframe_t *iframe, uint32_t exception_flags, uint32_t esr);
  32 | void arm64_irq_exception(iframe_t *iframe, uint32_t exception_flags);
  33 | 
  34 | #endif /* ABOS_EXCEPTIONS_H */
```
### 12.5.3 Technical Notes

1. **Strict Binary Coupling with `exceptions.S`**: Assembly macros in `kernel/core/exceptions.S` allocate `sub sp, sp, #288` and store register pairs (`stp x0, x1, [sp, #0]` ... `stp x28, x29, [sp, #224]`, plus `lr`, `sp_el0`, `elr_el1`, `spsr_el1`, `mdscr_el1`). The trailing `uint64_t pad` at offset 280 ensures `sizeof(iframe_t) == 288`, an exact multiple of 16 bytes, satisfying AArch64's hardware `SP` alignment check (`SCTLR_EL1.SA`).

---

## 12.6 include/gic.h

### 12.6.1 Functional Description

`include/gic.h` (32 lines) maps the ARM GICv2 Distributor (`GICD_*`) and per-core CPU Interface (`GICC_*`) MMIO registers through `KERNEL_PERIPHMAP_OFFSET` and declares the interrupt routing, masking, acknowledge, and end-of-interrupt primitives.

### 12.6.2 Heavily Commented Source Code

#### Program 12-5: `lesson_20/include/gic.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/gic.h" start="1" end="32" total="32" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/gic.h (32 lines)
       //// ==========================================================================
       //// Defines higher-half MMIO register mappings and driver APIs for the ARM
       //// Generic Interrupt Controller v2 (GIC-400) on the QEMU virt machine.
   1 | /* =========================================================================
   2 |  * gic.h - ARM GICv2 Driver Using Higher-Half Periphmap Virtual Addresses
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_GIC_H
   5 | #define ABOS_GIC_H
   6 | 
   7 | #include <stdint.h>
   8 | #include "mmu.h"
   9 | 
       //// On QEMU virt, GICv2 Distributor lives at PA 0x08000000 and CPU Interface
       //// at PA 0x08010000. Adding KERNEL_PERIPHMAP_OFFSET (0xFFFFFFFFC0000000)
       //// places them in the Device-nGnRnE TTBR1_EL1 peripheral window.
  10 | #define GICD_BASE (KERNEL_PERIPHMAP_OFFSET + 0x08000000ULL)
  11 | #define GICC_BASE (KERNEL_PERIPHMAP_OFFSET + 0x08010000ULL)
  12 | 
       //// Global GIC Distributor (GICD) registers:
       ////   - Scalar registers (CTLR, TYPER) dereference volatile uint32_t* directly.
       ////   - Banked bitmask/byte arrays (ISENABLER, ICENABLER, IPRIORITYR,
       ////     ITARGETSR) evaluate to volatile uint32_t* base pointers indexed by
       ////     IRQ word (int_id / 32 or int_id / 4) in gic.c.
  13 | #define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
  14 | #define GICD_TYPER      (*(volatile uint32_t *)(GICD_BASE + 0x004))
  15 | #define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))
  16 | #define GICD_ICENABLER  ((volatile uint32_t *)(GICD_BASE + 0x180))
  17 | #define GICD_IPRIORITYR ((volatile uint32_t *)(GICD_BASE + 0x400))
  18 | #define GICD_ITARGETSR  ((volatile uint32_t *)(GICD_BASE + 0x800))
  19 | 
       //// Per-CPU GIC Interface (GICC) registers and IRQ lifecycle prototypes:
       //// GICC_IAR acknowledges and returns the active interrupt ID; GICC_EOIR
       //// signals end-of-interrupt completion back to the GIC hardware.
  20 | #define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x0000))
  21 | #define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x0004))
  22 | #define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x000C))
  23 | #define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x0010))
  24 | 
  25 | void gic_dist_init(void);
  26 | void gic_cpu_init(void);
  27 | void gic_enable_interrupt(uint32_t int_id);
  28 | void gic_disable_interrupt(uint32_t int_id);
  29 | uint32_t gic_acknowledge_interrupt(void);
  30 | void gic_end_interrupt(uint32_t iar);
  31 | 
  32 | #endif /* ABOS_GIC_H */
```
### 12.6.3 Technical Notes

1. **Split Kernel Top-Half vs User-Space Bottom-Half**: When a peripheral SPI (such as UART0 IRQ 33 or virtio-blk) fires, `arm64_irq_exception()` reads `GICC_IAR`, masks the SPI in `GICD_ICENABLER` via `ab_interrupt_dispatch_hw_irq()`, signals the `InterruptDispatcher` event, and writes `GICC_EOIR` immediately so other interrupts are not blocked—deferring actual device service to the `EL0` driver thread until it calls `SYS_INTERRUPT_ACK` to re-set `GICD_ISENABLER`.

---

## 12.7 include/timer.h

### 12.7.1 Functional Description

`include/timer.h` (21 lines) defines the ARMv8 Generic Physical Timer PPI constant (`TIMER_ARCH_PHYS_IRQ = 30`), the 64-bit nanosecond timestamp type `ab_time_t`, and the per-CPU tick and monotonic clock functions.

### 12.7.2 Heavily Commented Source Code

#### Program 12-6: `lesson_20/include/timer.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/timer.h" start="1" end="21" total="21" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/timer.h (21 lines)
       //// ==========================================================================
       //// Declares the ARMv8 Generic Timer per-CPU tick driver and the system-wide
       //// 64-bit monotonic nanosecond clock (ab_time_t).
   1 | /* =========================================================================
   2 |  * timer.h - ARMv8 Generic Timer & Monotonic Clock (zx_time_t)
   3 |  * References:
   4 |  *   - zircon_org/kernel/dev/timer/arm_generic/arm_generic_timer.cpp
   5 |  * ========================================================================= */
   6 | #ifndef ABOS_TIMER_H
   7 | #define ABOS_TIMER_H
   8 | 
   9 | #include <stdint.h>
  10 | 
       //// PPI 30 is the architectural GIC interrupt ID for the ARMv8 Non-Secure
       //// Physical Timer (CNTP_CTL_EL0 / CNTP_TVAL_EL0) on every Cortex-A core.
       //// ab_time_t mirrors Zircon's zx_time_t: unsigned nanoseconds since boot
       //// (at 64 bits, 2^64 ns spans ~584.9 years without wraparound).
  11 | /* ARM Non-Secure Physical Timer PPI ID on QEMU virt */
  12 | #define TIMER_ARCH_PHYS_IRQ 30
  13 | 
  14 | typedef uint64_t ab_time_t;
  15 | 
       //// Per-CPU initialization (100 Hz preemption tick), IRQ top-half handler,
       //// per-core tick diagnostics, and high-resolution current_time_ns() reader
       //// (computed directly from CNTPCT_EL0 and CNTFRQ_EL0).
  16 | void timer_init_percpu(uint32_t hz);
  17 | void timer_irq_handler(void);
  18 | uint64_t timer_get_ticks(uint32_t cpu);
  19 | ab_time_t current_time_ns(void);
  20 | 
  21 | #endif /* ABOS_TIMER_H */
```
### 12.7.3 Technical Notes

1. **Hardware Counter Precision vs Tick Granularity**: Although `timer_init_percpu(100)` schedules preemption ticks at 100 Hz (every 10 ms), `current_time_ns()` reads the hardware system counter `CNTPCT_EL0` scaled by `CNTFRQ_EL0` (62.5 MHz on QEMU `virt`, 16 ns resolution), giving `ab_port_packet_t.timestamp` and `thread_sleep_ns` sub-microsecond accuracy.

---

## 12.8 include/pmm.h

### 12.8.1 Functional Description

`include/pmm.h` (48 lines) defines ABOS's physical page frame allocator types: 4 KB page geometry, the 7-state `vm_page_state_t` accounting enum, the 16-byte `vm_page_t` frame metadata entry, and page allocation/reclamation routines.

### 12.8.2 Heavily Commented Source Code

#### Program 12-7: `lesson_20/include/pmm.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/pmm.h" start="1" end="48" total="48" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/pmm.h (48 lines)
       //// ==========================================================================
       //// Defines physical page constants, Zircon-style page accounting states,
       //// the per-frame vm_page_t metadata struct, and Physical Memory Manager APIs.
   1 | /* =========================================================================
   2 |  * pmm.h - Zircon-Style Physical Memory Manager (Higher-Half Physmap)
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_PMM_H
   5 | #define ABOS_PMM_H
   6 | 
   7 | #include <stdint.h>
   8 | #include <stddef.h>
   9 | 
       //// 4 KB base page geometry and static frame table capacity:
       //// PMM_MAX_PAGES = 32,768 pages * 4,096 B/page = 128 MB of managed RAM.
  10 | #define PAGE_SIZE       4096UL
  11 | #define PAGE_SHIFT      12
  12 | #define PMM_MAX_PAGES   32768U
  13 | 
  14 | typedef uint64_t paddr_t;
  15 | 
       //// Zircon-inspired page state taxonomy (vm_page_state_t): every physical
       //// 4 KB frame is tagged with its exact owner role — FREE, generic ALLOC,
       //// VMO OBJECT page, kernel WIRED image/DTB/stack page, kernel HEAP backing
       //// page, MMU L1/L2/L3 page-table page, or IPC buffer page.
  16 | typedef enum {
  17 |     VM_PAGE_STATE_FREE = 0,
  18 |     VM_PAGE_STATE_ALLOC,
  19 |     VM_PAGE_STATE_OBJECT,
  20 |     VM_PAGE_STATE_WIRED,
  21 |     VM_PAGE_STATE_HEAP,
  22 |     VM_PAGE_STATE_MMU,
  23 |     VM_PAGE_STATE_IPC,
  24 |     VM_PAGE_STATE_COUNT
  25 | } vm_page_state_t;
  26 | 
       //// Compact 16-byte per-page descriptor (32,768 * 16 B = 512 KB total BSS):
       ////   - paddr         (8B) : Physical base address of this 4 KB frame.
       ////   - next_free_idx (4B) : Singly-linked free-list index (avoids 8B ptr).
       ////   - state         (1B) : Current vm_page_state_t tag.
       ////   - pin_count     (1B) : DMA / wired pin count.
       ////   - flags         (2B) : Reserved per-frame attribute flags.
  27 | typedef struct vm_page {
  28 |     paddr_t  paddr;
  29 |     uint32_t next_free_idx;
  30 |     uint8_t  state;
  31 |     uint8_t  pin_count;
  32 |     uint16_t flags;
  33 | } vm_page_t;
  34 | 
       //// PMM public interface: boot-time arena initialization (carving out kernel
       //// image, DTB, and initrd), O(1) single-page allocation, contiguous
       //// multi-page allocation for virtio DMA, pmm_free_wired_range() to reclaim
       //// the bootloader initrd once copied into the BOOTFS VMO, and diagnostics.
  35 | void pmm_init(paddr_t ram_base, uint64_t ram_size, paddr_t kernel_end_pa,
  36 |               paddr_t dtb_pa, uint32_t dtb_size,
  37 |               paddr_t initrd_start, paddr_t initrd_end);
  38 | vm_page_t *pmm_alloc_page(vm_page_state_t state, paddr_t *out_paddr);
  39 | vm_page_t *pmm_alloc_contiguous(size_t count, vm_page_state_t state, paddr_t *out_paddr);
  40 | void pmm_free_page(paddr_t paddr);
  41 | void pmm_free_wired_range(paddr_t start_pa, paddr_t end_pa);
  42 | vm_page_t *pmm_paddr_to_page(paddr_t paddr);
  43 | uint64_t pmm_count_free_pages(void);
  44 | uint64_t pmm_count_total_pages(void);
  45 | void pmm_dump_state_counts(void);
  46 | const char *vm_page_state_name(vm_page_state_t state);
  47 | 
  48 | #endif /* ABOS_PMM_H */
```
### 12.8.3 Technical Notes

1. **Zero-Padding `vm_page_t` Layout**: Ordering fields from largest to smallest alignment (`uint64_t paddr` [8B], `uint32_t next_free_idx` [4B], `uint8_t state` [1B], `uint8_t pin_count` [1B], `uint16_t flags` [2B]) packs `vm_page_t` into **16 bytes** with zero compiler padding. Indexing `next_free_idx` as a 32-bit array index rather than a 64-bit pointer saves 8 bytes per frame across all 32,768 pages (256 KB saved).

---

## 12.9 include/heap.h

### 12.9.1 Functional Description

`include/heap.h` (19 lines) declares the general-purpose kernel heap interface (`heap_init`, `kmalloc`, `kzalloc`, `kfree`, and `heap_dump_stats`) used to allocate variable-sized kernel structures such as `vm_aspace_t`, `ab_object_t` dispatchers, `ab_message_packet_t` nodes, and syscall introspection buffers.

### 12.9.2 Heavily Commented Source Code

#### Program 12-8: `lesson_20/include/heap.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/heap.h" start="1" end="19" total="19" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/heap.h (19 lines)
       //// ==========================================================================
       //// Declares the sub-page dynamic kernel heap allocator layered atop
       //// PMM pages tagged with VM_PAGE_STATE_HEAP.
   1 | /* =========================================================================
   2 |  * heap.h - Kernel Heap Allocator (kmalloc / kzalloc / kfree)
   3 |  * References:
   4 |  *   - zircon_org/kernel/lib/heap/include/lib/heap.h
   5 |  *   - zircon_org/kernel/lib/heap/cmpctmalloc/cmpctmalloc.c
   6 |  * ========================================================================= */
   7 | #ifndef ABOS_HEAP_H
   8 | #define ABOS_HEAP_H
   9 | 
  10 | #include <stddef.h>
  11 | #include <stdint.h>
  12 | 
       //// Core kernel heap primitives:
       ////   - heap_init()       : Initializes the free-block arena after pmm_init().
       ////   - kmalloc(size)     : 16-byte-aligned uninitialized kernel allocation.
       ////   - kzalloc(size)     : Zero-filled allocation — mandatory for all
       ////                         ab_object_t dispatchers and syscall staging
       ////                         buffers so uninitialized heap bytes never leak!
       ////   - kfree(ptr)        : Returns block to heap and coalesces neighbors.
       ////   - heap_dump_stats() : Prints live allocated/free byte counters.
  13 | void heap_init(void);
  14 | void *kmalloc(size_t size);
  15 | void *kzalloc(size_t size);
  16 | void kfree(void *ptr);
  17 | void heap_dump_stats(void);
  18 | 
  19 | #endif /* ABOS_HEAP_H */
```
### 12.9.3 Technical Notes

1. **Security Role of `kzalloc()`**: Because C structs frequently contain implicit alignment padding (for instance, between `uint32_t type` and `uint64_t timestamp` in `ab_port_packet_t`, or inside `ab_task_info_entry_t`), allocating kernel objects and staging buffers with `kzalloc()` guarantees zeroed padding and prevents residual kernel pointers from leaking across `copy_to_user()`.

---

## 12.10 include/mmu.h

### 12.10.1 Functional Description

`include/mmu.h` (84 lines) specifies the virtual memory architecture of ABOS: the higher-half `TTBR1_EL1` linear `physmap` and `periphmap` offsets and inline converters, ARMv8-A 4 KB Stage-1 page-table descriptor flags (`PTE_*`), portable VMM flags (`MMU_FLAG_*`), the 2 GB `TTBR0_EL1` user address space bounds (`USER_ASPACE_*`), and the ASID-tagged `vm_aspace_t` object.

### 12.10.2 Heavily Commented Source Code

#### Program 12-9: `lesson_20/include/mmu.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/mmu.h" start="1" end="84" total="84" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/mmu.h (84 lines)
       //// ==========================================================================
       //// Defines ABOS's dual-TTBR virtual memory layout, higher-half physmap and
       //// periphmap translation helpers, ARMv8-A Stage-1 PTE descriptor bits, and
       //// per-process ASID-tagged VmAspace structures.
   1 | /* =========================================================================
   2 |  * mmu.h - AArch64 MMU, Kernel Physmap/Periphmap & Zircon-Style VmAspace
   3 |  * References:
   4 |  *   - zircon_org/kernel/arch/arm64/mmu.cpp
   5 |  *   - zircon_org/kernel/vm/include/vm/vm_aspace.h
   6 |  *   - zircon_org/kernel/vm/include/vm/physmap.h
   7 |  * ========================================================================= */
   8 | #ifndef ABOS_MMU_H
   9 | #define ABOS_MMU_H
  10 | 
  11 | #include <stdint.h>
  12 | #include <stddef.h>
  13 | #include "pmm.h"
  14 | #include "spinlock.h"
  15 | 
  16 | typedef uint64_t vaddr_t;
  17 | typedef uint64_t pte_t;
  18 | 
       //// Higher-Half Kernel Virtual Memory Map (TTBR1_EL1):
       ////   +---------------------------------------+------------------------------+
       ////   | Kernel Virtual Range                  | Physical Address Range       |
       ////   +---------------------------------------+------------------------------+
       ////   | 0xFFFFFFFF80000000..0xFFFFFFFFBFFFFFFF| 0x40000000..0x7FFFFFFF (RAM) |
       ////   | 0xFFFFFFFFC0000000..0xFFFFFFFFFFFFFFFF| 0x00000000..0x3FFFFFFF (MMIO)|
       ////   +---------------------------------------+------------------------------+
       //// WHY KERNEL_PHYSMAP_OFFSET IS 0xFFFFFFFF40000000: QEMU virt RAM starts at
       //// PA 0x40000000 (1 GB). Adding 0xFFFFFFFF40000000 maps PA 0x40000000
       //// directly to VA 0xFFFFFFFF80000000 with a single 64-bit ADD/SUB!
  19 | /*
  20 |  * Higher-Half Kernel Address Space Layout (TTBR1_EL1):
  21 |  *   0xFFFFFFFF80000000 .. 0xFFFFFFFFBFFFFFFF : 1GB Physmap -> PA 0x40000000..0x7FFFFFFF
  22 |  *   0xFFFFFFFFC0000000 .. 0xFFFFFFFFFFFFFFFF : 1GB Periphmap -> PA 0x00000000..0x3FFFFFFF
  23 |  */
  24 | #define KERNEL_PHYSMAP_OFFSET   0xFFFFFFFF40000000ULL
  25 | #define KERNEL_PERIPHMAP_OFFSET 0xFFFFFFFFC0000000ULL
  26 | 
  27 | static inline void *paddr_to_physmap(paddr_t pa) {
  28 |     return (void *)(uintptr_t)(pa + KERNEL_PHYSMAP_OFFSET);
  29 | }
  30 | 
  31 | static inline paddr_t physmap_to_paddr(const void *va) {
  32 |     return (paddr_t)((uintptr_t)va - KERNEL_PHYSMAP_OFFSET);
  33 | }
  34 | 
  35 | static inline void *paddr_to_periphmap(paddr_t pa) {
  36 |     return (void *)(uintptr_t)(pa + KERNEL_PERIPHMAP_OFFSET);
  37 | }
  38 | 
       //// ARMv8-A 4 KB Granule Stage-1 Page/Table Descriptor Bits:
       ////   - PTE_VALID / PTE_TABLE / PTE_PAGE : Bits [1:0] = 0b11 for L1/L2/L3.
       ////   - PTE_ATTR_DEVICE / NORMAL         : MAIR_EL1 index 0 vs 1 in bits [4:2].
       ////   - PTE_AP_USER (bit 6), PTE_AP_RO (bit 7) : Stage-1 Access Permissions.
       ////   - PTE_SH_INNER (bits [9:8]=0b11)   : SMP Inner-Shareable coherency.
       ////   - PTE_AF (bit 10)                  : Access Flag pre-set to avoid faults.
       ////   - PTE_NG (bit 11)                  : Non-Global (tags TLB entry w/ ASID!
       ////                                        Crucial so switching processes does
       ////                                        NOT flush the entire TLB).
       ////   - PTE_PXN (bit 53), PTE_UXN (bit 54): Hardware W^X & SMEP-equivalent
       ////                                        (EL1 never executes user pages).
  39 | /* AArch64 Stage-1 Page/Table Descriptor Bits */
  40 | #define PTE_VALID           (1ULL << 0)
  41 | #define PTE_TABLE           (1ULL << 1) /* For L1/L2 table descriptors */
  42 | #define PTE_PAGE            (1ULL << 1) /* For L3 4KB page descriptors */
  43 | #define PTE_ATTR_DEVICE     (0ULL << 2) /* MAIR AttrIdx 0 = Device-nGnRnE */
  44 | #define PTE_ATTR_NORMAL     (1ULL << 2) /* MAIR AttrIdx 1 = Normal WBWA */
  45 | #define PTE_AP_USER         (1ULL << 6) /* AP[1]=1: EL0 accessible */
  46 | #define PTE_AP_RO           (1ULL << 7) /* AP[2]=1: Read-only */
  47 | #define PTE_SH_INNER        (3ULL << 8) /* Inner Shareable */
  48 | #define PTE_AF              (1ULL << 10)/* Access Flag */
  49 | #define PTE_NG              (1ULL << 11)/* Non-Global (ASID-tagged TLB entry) */
  50 | #define PTE_PXN             (1ULL << 53)/* Privileged Execute Never */
  51 | #define PTE_UXN             (1ULL << 54)/* Unprivileged Execute Never */
  52 | 
  53 | #define PTE_ADDR_MASK       0x0000FFFFFFFFF000ULL
  54 | 
       //// Architecture-neutral VMM permission flags and the 2 GB TTBR0_EL1 user
       //// address window [0x1000, 0x80000000) (leaving 0x0..0xFFF unmapped to
       //// catch NULL pointer dereferences immediately).
  55 | /* High-level mapping permission flags (modeling Zircon's ARCH_MMU_FLAG_*) */
  56 | #define MMU_FLAG_PERM_READ      (1U << 0)
  57 | #define MMU_FLAG_PERM_WRITE     (1U << 1)
  58 | #define MMU_FLAG_PERM_EXECUTE   (1U << 2)
  59 | #define MMU_FLAG_PERM_USER      (1U << 3)
  60 | #define MMU_FLAG_CACHE_DEVICE   (1U << 4)
  61 | 
  62 | /* User space virtual address range (TTBR0_EL1 with T0SZ=33 -> 0 .. 2GB) */
  63 | #define USER_ASPACE_BASE        0x0000000000001000ULL
  64 | #define USER_ASPACE_SIZE        0x000000007FFFF000ULL
  65 | 
       //// vm_aspace_t represents a process's hardware address space: protected by
       //// a spinlock, it tracks the physical L1 table address for TTBR0_EL1, its
       //// physmap virtual pointer for fast software walks, its 8-bit/16-bit ASID,
       //// and live mapped page count.
  66 | typedef struct vm_aspace {
  67 |     spin_lock_t lock;
  68 |     paddr_t     ttbr0_phys;    /* Physical address of L1 page table */
  69 |     pte_t      *l1_virt;       /* Kernel physmap pointer to L1 table */
  70 |     uint16_t    asid;
  71 |     uint32_t    mapped_pages;
  72 | } vm_aspace_t;
  73 | 
  74 | void mmu_init(void);
  75 | uint16_t ab_alloc_asid(void);
  76 | void ab_free_asid(uint16_t asid);
  77 | vm_aspace_t *vmm_create_user_aspace(uint16_t asid);
  78 | int vmm_map_page(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags);
  79 | int vmm_unmap_page(vm_aspace_t *aspace, vaddr_t vaddr);
  80 | int vmm_resolve_vaddr(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t *out_paddr, pte_t *out_pte);
  81 | void vmm_switch_aspace(vm_aspace_t *aspace);
  82 | void vmm_destroy_aspace(vm_aspace_t *aspace);
  83 | 
  84 | #endif /* ABOS_MMU_H */
```
### 12.10.3 Technical Notes

1. **Why `PTE_NG` + `ASID` Makes Microkernel IPC Fast**: In a monolithic kernel, context switches between kernel threads rarely change `TTBR0_EL1`; in a microkernel, every synchronous `shell -> console-srv` or `shell -> fs-srv` RPC switches processes! Marking every user PTE with `PTE_NG` (Non-Global, bit 11) tags cached TLB entries with `vm_aspace_t.asid` (`TTBR0_EL1[63:48]`), allowing `vmm_switch_aspace()` to switch address spaces with a single `msr ttbr0_el1` + `isb` and **zero** TLB flushes.

---

## 12.11 include/thread.h

### 12.11.1 Functional Description

`include/thread.h` (90 lines) defines the LK kernel threading layer: the 6-state `thread_state_t` enum, the 112-byte AAPCS64 `context_switch_frame_t` saved by `arm64_context_switch` in `switch.S`, the `thread_t` Thread Control Block (including its 3 pointer hooks upward into Zircon's `VmAspace`, `ProcessDispatcher`, and `ThreadDispatcher`), and the SMP scheduler API.

### 12.11.2 Heavily Commented Source Code

#### Program 12-10: `lesson_20/include/thread.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/thread.h" start="1" end="90" total="90" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/thread.h (90 lines)
       //// ==========================================================================
       //// Defines the lower-layer LK execution engine: per-thread 8 KB kernel
       //// stacks, AAPCS64 callee-saved context-switch frames, thread_t TCBs,
       //// and preemptive SMP scheduler interfaces.
   1 | /* =========================================================================
   2 |  * thread.h - LK Kernel Thread (thread_t) & Preemptive Scheduler
   3 |  * References:
   4 |  *   - zircon_org/kernel/include/kernel/thread.h
   5 |  *   - zircon_org/kernel/arch/arm64/include/arch/arch_thread.h
   6 |  * ========================================================================= */
   7 | #ifndef ABOS_THREAD_H
   8 | #define ABOS_THREAD_H
   9 | 
  10 | #include <stdint.h>
  11 | #include <stddef.h>
  12 | #include "mmu.h"
  13 | #include "timer.h"
  14 | 
       //// Fixed scheduler table capacity (64 threads) and 8 KB per-thread EL1
       //// stack size (2 * 4 KB pages). Keeping kernel stacks compact saves wired
       //// RAM, which is why syscalls.c allocates multi-KB ps/vmaps arrays on heap.
  15 | #define MAX_THREADS       64
  16 | #define THREAD_STACK_SIZE (2 * PAGE_SIZE) /* 8KB per kernel thread stack */
  17 | 
       //// Six-state LK thread lifecycle machine:
       //// INITIAL -> READY <-> RUNNING -> (BLOCKED | SLEEPING) -> READY -> DEATH.
  18 | typedef enum {
  19 |     THREAD_INITIAL = 0,
  20 |     THREAD_READY,
  21 |     THREAD_RUNNING,
  22 |     THREAD_BLOCKED,
  23 |     THREAD_SLEEPING,
  24 |     THREAD_DEATH
  25 | } thread_state_t;
  26 | 
  27 | typedef int (*thread_start_routine)(void *arg);
  28 | 
       //// context_switch_frame_t (112 bytes) pushed onto kernel_sp by switch.S:
       //// Saves ONLY AAPCS64 callee-saved registers (x19..x29, x30/lr) plus the
       //// two EL0 thread-local storage system registers (TPIDR_EL0, TPIDRRO_EL0).
       //// Caller-saved registers x0..x18 live either in C caller frames or (for
       //// preempted/trapped threads) inside the 288-byte iframe_t above!
  29 | /* Matches struct context_switch_frame in zircon_org/kernel/arch/arm64/thread.cpp */
  30 | typedef struct arm64_context_switch_frame {
  31 |     uint64_t tpidr_el0;
  32 |     uint64_t tpidrro_el0;
  33 |     uint64_t r19;
  34 |     uint64_t r20;
  35 |     uint64_t r21;
  36 |     uint64_t r22;
  37 |     uint64_t r23;
  38 |     uint64_t r24;
  39 |     uint64_t r25;
  40 |     uint64_t r26;
  41 |     uint64_t r27;
  42 |     uint64_t r28;
  43 |     uint64_t r29;
  44 |     uint64_t lr;
  45 | } context_switch_frame_t;
  46 | 
       //// Thread Control Block (thread_t):
       ////   - Core LK scheduling fields : tid, name, state, pinned_cpu, kernel_sp,
       ////     stack_base, stack_size, entry, arg, retcode.
       ////   - Wait/Timer fields         : wake_time_ns, wait_status, blocking_wq,
       ////     intrusive wait_next pointer, and detached reaping flag.
       ////   - Zircon Upper-Layer Bridge (lines 63-65): aspace (loaded into
       ////     TTBR0_EL1 on every context switch!), process_obj (ab_process_t*),
       ////     and thread_obj (ab_thread_dispatcher_t*).
  47 | typedef struct thread {
  48 |     uint32_t             tid;
  49 |     char                 name[32];
  50 |     volatile thread_state_t state;
  51 |     int                  pinned_cpu;     /* -1 = any CPU, 0..3 = pinned */
  52 |     vaddr_t              kernel_sp;      /* Saved SP for arm64_context_switch */
  53 |     vaddr_t              stack_base;     /* Bottom of 8KB kernel stack */
  54 |     size_t               stack_size;
  55 |     thread_start_routine entry;
  56 |     void                *arg;
  57 |     int                  retcode;
  58 |     ab_time_t            wake_time_ns;   /* Deadline for SLEEPING / timed BLOCKED */
  59 |     int                  wait_status;    /* Status returned when woken from wait_queue */
  60 |     struct wait_queue   *blocking_wq;    /* Wait queue this thread is currently on */
  61 |     struct thread       *wait_next;      /* Intrusive link inside wait_queue_t */
  62 |     int                  detached;       /* 1 = ready for kernel stack/struct reaping on DEATH */
  63 |     vm_aspace_t         *aspace;         /* NULL for kernel threads; user VmAspace for EL0 */
  64 |     void                *process_obj;    /* ProcessDispatcher pointer (Lesson 07+) */
  65 |     void                *thread_obj;     /* ThreadDispatcher pointer (Lesson 07+) */
  66 | } thread_t;
  67 | 
       //// Assembly context-switch routine, public thread lifecycle/sleep/preempt
       //// primitives, and internal global scheduler lock hooks shared exclusively
       //// with wait.c to make block-and-reschedule transitions atomic.
  68 | extern void arm64_context_switch(vaddr_t *old_sp, vaddr_t new_sp);
  69 | 
  70 | void thread_init_early(void);
  71 | void thread_secondary_cpu_init(void);
  72 | thread_t *get_current_thread(void);
  73 | thread_t *thread_create(const char *name, thread_start_routine entry, void *arg);
  74 | void thread_detach(thread_t *t);
  75 | void thread_reap_dead(void);
  76 | void thread_set_pinned_cpu(thread_t *t, int cpu);
  77 | void thread_resume(thread_t *t);
  78 | void thread_yield(void);
  79 | void thread_preempt(void);
  80 | void thread_sleep_ns(ab_time_t delay_ns);
  81 | void thread_exit(int retcode);
  82 | void thread_timer_tick(void);
  83 | void thread_become_idle(void);
  84 | 
  85 | /* Internal scheduler lock helpers used by wait_queue_t (Lesson 06+) */
  86 | void sched_lock_acquire(uint64_t *irqstate_out);
  87 | void sched_lock_release(uint64_t irqstate);
  88 | void sched_reschedule_locked(void);
  89 | 
  90 | #endif /* ABOS_THREAD_H */
```
### 12.11.3 Technical Notes

1. **Clean Two-Layer Decoupling via Opaque Pointers**: Notice lines 64–65 of `thread.h` type `process_obj` and `thread_obj` as `void *` rather than `#include "object.h"`. This deliberate decoupling preserves the LK/Zircon two-layer boundary: the LK scheduler (`thread.c`, `wait.c`) schedules pure `thread_t` instances and switches `vm_aspace_t *aspace` without knowing anything about Zircon handles, channels, or capability rights!

---

## 12.12 include/wait.h

### 12.12.1 Functional Description

`include/wait.h` (43 lines) builds FIFO thread wait queues (`wait_queue_t`) and level/auto-unsignal synchronization events (`event_t`) directly atop `thread.h`, supporting both infinite (`AB_TIME_INFINITE = 0`) and absolute nanosecond deadlines.

### 12.12.2 Heavily Commented Source Code

#### Program 12-11: `lesson_20/include/wait.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/wait.h" start="1" end="43" total="43" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/wait.h (43 lines)
       //// ==========================================================================
       //// Defines LK wait queues (wait_queue_t) and kernel events (event_t), the
       //// foundational blocking primitives underneath Zircon StateTrackers, Ports,
       //// and InterruptDispatchers.
   1 | /* =========================================================================
   2 |  * wait.h - LK Wait Queues (wait_queue_t) & Events (event_t)
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_WAIT_H
   5 | #define ABOS_WAIT_H
   6 | 
   7 | #include <stdint.h>
   8 | #include "thread.h"
   9 | 
       //// Event behavior flags, infinite deadline sentinel (0), and guarded
       //// status constants (#ifndef AB_OK) allowing wait.h to be included
       //// independently before object.h without macro redefinition conflicts.
  10 | #define EVENT_FLAG_AUTOUNSIGNAL (1U << 0)
  11 | #define AB_TIME_INFINITE        ((ab_time_t)0)
  12 | 
  13 | #ifndef AB_OK
  14 | #define AB_OK                 0
  15 | #define AB_ERR_TIMED_OUT     -21
  16 | #define AB_ERR_CANCELED      -23
  17 | #endif
  18 | 
       //// Intrusive O(1) FIFO wait_queue_t (threads link via thread_t.wait_next
       //// with zero dynamic memory allocation!) and event_t (combining a volatile
       //// signaled latch, auto-unsignal flag, and embedded wait_queue_t).
  19 | typedef struct wait_queue {
  20 |     thread_t *head;
  21 |     thread_t *tail;
  22 |     uint32_t  count;
  23 | } wait_queue_t;
  24 | 
  25 | typedef struct event {
  26 |     volatile int signaled;
  27 |     uint32_t     flags;
  28 |     wait_queue_t wait;
  29 | } event_t;
  30 | 
       //// Wait-queue primitives (suffixed _locked because callers must hold the
       //// scheduler lock to prevent lost-wakeup races between checking a condition
       //// and transitioning to THREAD_BLOCKED) and high-level event_wait/signal.
  31 | void wait_queue_init(wait_queue_t *wq);
  32 | void wait_queue_remove_thread_locked(wait_queue_t *wq, thread_t *target);
  33 | int  wait_queue_block_locked(wait_queue_t *wq, ab_time_t deadline_ns);
  34 | int  wait_queue_wake_one_locked(wait_queue_t *wq, int wait_status);
  35 | int  wait_queue_wake_all_locked(wait_queue_t *wq, int wait_status);
  36 | 
  37 | void event_init(event_t *e, int initial, uint32_t flags);
  38 | int  event_wait_deadline(event_t *e, ab_time_t deadline_ns);
  39 | int  event_wait(event_t *e);
  40 | int  event_signal(event_t *e);
  41 | void event_unsignal(event_t *e);
  42 | 
  43 | #endif /* ABOS_WAIT_H */
```
### 12.12.3 Technical Notes

1. **Auto-Unsignal vs Broadcast Events**: When `flags` includes `EVENT_FLAG_AUTOUNSIGNAL` (used for one-thread wakeup semantics), `event_signal()` wakes a single waiting thread and atomically clears `signaled` back to `0`. When `flags == 0` (used by `ab_wait_observer_t` and `PortDispatcher.readable_event` when controlled manually), `event_signal()` latches `signaled = 1` and wakes waiters until explicitly reset via `event_unsignal()`.

---

## 12.13 include/object.h

### 12.13.1 Functional Description

Spanning 590 lines, `include/object.h` is the architectural heart of ABOS's Zircon layer. Rather than scattering interdependent object types across a dozen tiny files, `object.h` presents the complete capability object model in dependency order:
- **Status Codes, Object Types, Capability Rights & Signals (Lines 19–103)**: Defines `ab_status_t` (`AB_OK` and 17 `AB_ERR_*` codes), the 12 `ab_obj_type_t` kernel object kinds, the 32-bit `AB_RIGHT_*` capability rights algebra, and the 32-bit `AB_SIGNAL_*` readiness/user signal bitmask.
- **Observer, StateTracker & Base Dispatcher `ab_object_t` (Lines 104–156)**: Defines `ab_wait_observer_t` (bridging synchronous thread waits and asynchronous `Port` packet delivery), `ab_state_tracker_t`, `ab_handle_basic_info_t`, the polymorphic base `ab_object_t` with split `ref_count` / `handle_count` and `on_zero_handles` / `destroy` hooks, and generation-counted `ab_handle_entry_t` handle slots.
- **Memory & Core Dispatchers (Lines 157–275)**: Defines `ab_vmo_dispatcher_t`, `ab_vmar_dispatcher_t`, `ab_vmar_mapping_t`, `ab_process_t`, `ab_log_dispatcher_t`, `ab_event_dispatcher_t`, and `ab_eventpair_dispatcher_t`, alongside core object/signal/handle prototypes.
- **Ports & Channels (Lines 277–381)**: Defines `ab_port_packet_t`, the 32-slot ring buffer `ab_port_dispatcher_t`, capability-carrying `ab_message_packet_t`, entangled `ab_channel_dispatcher_t`, and the `ab_channel_write_args_t` / `ab_channel_read_args_t` syscall descriptors.
- **VMO/VMAR, Tasks, Driver DDK & Introspection (Lines 383–590)**: Defines `ab_vmar_map_args_t`, `ab_job_dispatcher_t`, `ab_thread_dispatcher_t`, `ab_resource_dispatcher_t`, `ab_interrupt_dispatcher_t`, physical/contiguous DMA descriptors, and the `ps` (`ab_task_info_entry_t`) and `vmaps` (`ab_info_maps_t`) diagnostic schemas.

### 12.13.2 Heavily Commented Source Code

#### Program 12-12: `lesson_20/include/object.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/object.h" start="1" end="590" total="590" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/object.h (590 lines)
       //// ==========================================================================
       //// The centerpiece header of the ABOS microkernel — shared between EL1
       //// kernel subsystems and EL0 ulibc. Across 590 lines, it defines:
       ////   1. Core ABI types, KOIDs, and Zircon status codes     (lines  19-48)
       ////   2. Object types, capability rights, and signal masks  (lines  49-103)
       ////   3. Wait observers, StateTracker, and base ab_object_t (lines 104-156)
       ////   4. VMO, VMAR, Process, Log, Event & EventPair structs (lines 157-275)
       ////   5. PortDispatcher & async wait packet structures      (lines 277-318)
       ////   6. ChannelDispatcher & capability-transfer messages   (lines 320-381)
       ////   7. VMO/VMAR mapping descriptors & fault handler APIs  (lines 383-408)
       ////   8. Task hierarchy (Job/Process/Thread) & Driver DDK   (lines 410-517)
       ////   9. Diagnostics ('ps' & 'vmaps') introspection structs (lines 518-590)
   1 | /* =========================================================================
   2 |  * object.h - Zircon Kernel Objects (Dispatcher), StateTracker, Event & EventPair
   3 |  * References:
   4 |  *   - zircon_org/kernel/object/include/object/dispatcher.h
   5 |  *   - zircon_org/kernel/object/include/object/state_tracker.h
   6 |  *   - zircon_org/kernel/object/include/object/event_dispatcher.h
   7 |  *   - zircon_org/kernel/object/include/object/event_pair_dispatcher.h
   8 |  * ========================================================================= */
   9 | #ifndef ABOS_OBJECT_H
  10 | #define ABOS_OBJECT_H
  11 | 
  12 | #include <stdint.h>
  13 | #include <stddef.h>
  14 | #include "spinlock.h"
  15 | #include "mmu.h"
  16 | #include "thread.h"
  17 | #include "wait.h"
  18 | 
       //// Five canonical Zircon ABI scalar types:
       ////   - ab_status_t  : Signed 32-bit return code (0 = AB_OK, < 0 = error).
       ////   - ab_handle_t  : Opaque 32-bit process-local capability token.
       ////   - ab_koid_t    : Globally unique, never-reused 64-bit Kernel Object ID.
       ////   - ab_rights_t  : 32-bit bitmask of permitted operations on a handle.
       ////   - ab_signals_t : 32-bit bitmask of observable state assertions.
  19 | typedef int32_t  ab_status_t;
  20 | typedef uint32_t ab_handle_t;
  21 | typedef uint64_t ab_koid_t;
  22 | typedef uint32_t ab_rights_t;
  23 | typedef uint32_t ab_signals_t;
  24 | 
  25 | #define AB_HANDLE_INVALID       ((ab_handle_t)0U)
  26 | #define AB_KOID_INVALID         ((ab_koid_t)0ULL)
  27 | #define AB_KOID_KERNEL          ((ab_koid_t)1ULL)
  28 | 
       //// Zircon-compatible error status codes (modeled after zircon/errors.h):
       //// Grouped by failure class — resource exhaustion (-3, -4), bad caller
       //// inputs/handles/types (-10..-15), transient/lifecycle state (-20..-25),
       //// and capability security rejection (AB_ERR_ACCESS_DENIED = -30).
  29 | /* Zircon-style status codes (zircon_org/system/public/zircon/errors.h) */
  30 | #define AB_OK                    0
  31 | #define AB_ERR_INTERNAL         -1
  32 | #define AB_ERR_NOT_SUPPORTED    -2
  33 | #define AB_ERR_NO_RESOURCES     -3
  34 | #define AB_ERR_NO_MEMORY        -4
  35 | #define AB_ERR_INVALID_ARGS     -10
  36 | #define AB_ERR_BAD_HANDLE       -11
  37 | #define AB_ERR_WRONG_TYPE       -12
  38 | #define AB_ERR_BAD_SYSCALL      -13
  39 | #define AB_ERR_OUT_OF_RANGE     -14
  40 | #define AB_ERR_BUFFER_TOO_SMALL -15
  41 | #define AB_ERR_BAD_STATE        -20
  42 | #define AB_ERR_TIMED_OUT        -21
  43 | #define AB_ERR_SHOULD_WAIT      -22
  44 | #define AB_ERR_CANCELED         -23
  45 | #define AB_ERR_PEER_CLOSED      -24
  46 | #define AB_ERR_NOT_FOUND        -25
  47 | #define AB_ERR_ACCESS_DENIED    -30
  48 | 
       //// Enumeration of all 12 concrete kernel dispatcher types (1..12), plus
       //// AB_OBJ_TYPE_NONE (0, used as a wildcard in handle_lookup when any object
       //// type is acceptable, e.g., sys_object_wait_one or sys_object_get_info)
       //// and AB_OBJ_TYPE_COUNT (13, sizing per-type diagnostic arrays in 'ps').
  49 | /* Zircon Object Types (zircon_org/system/public/zircon/syscalls/object.h) */
  50 | typedef enum {
  51 |     AB_OBJ_TYPE_NONE = 0,
  52 |     AB_OBJ_TYPE_PROCESS,
  53 |     AB_OBJ_TYPE_THREAD,
  54 |     AB_OBJ_TYPE_VMO,
  55 |     AB_OBJ_TYPE_CHANNEL,
  56 |     AB_OBJ_TYPE_EVENT,
  57 |     AB_OBJ_TYPE_EVENTPAIR,
  58 |     AB_OBJ_TYPE_PORT,
  59 |     AB_OBJ_TYPE_INTERRUPT,
  60 |     AB_OBJ_TYPE_RESOURCE,
  61 |     AB_OBJ_TYPE_LOG,
  62 |     AB_OBJ_TYPE_JOB,
  63 |     AB_OBJ_TYPE_VMAR,
  64 |     AB_OBJ_TYPE_COUNT
  65 | } ab_obj_type_t;
  66 | 
       //// Capability Rights Bitmask (AB_RIGHT_*):
       //// Every handle pairs an ab_object_t* with a subset of these 14 rights:
       ////   - DUPLICATE / TRANSFER : Copy handle locally / move across a Channel.
       ////   - READ / WRITE / EXECUTE / MAP : Data I/O and VMAR page-table mapping.
       ////   - GET/SET_PROPERTY, INSPECT : Metadata & ps/vmaps introspection.
       ////   - SIGNAL / SIGNAL_PEER / WAIT : StateTracker synchronization.
       ////   - MANAGE_PROCESS / MANAGE_THREAD : Spawning tasks under Jobs/Processes.
       ////   - AB_RIGHT_SAME_RIGHTS (bit 31) : Sentinel for sys_handle_duplicate()
       ////     meaning 'preserve the source handle's exact rights bitmask'.
       ////
       //// Composite bundles (AB_RIGHTS_BASIC, DEFAULT_LOG, DEFAULT_EVENT,
       //// DEFAULT_EVENTPAIR) codify the initial rights granted at object creation.
  67 | /* Capability Rights Bitmask (zircon_org/system/public/zircon/rights.h) */
  68 | #define AB_RIGHT_NONE           ((ab_rights_t)0U)
  69 | #define AB_RIGHT_DUPLICATE      ((ab_rights_t)(1U << 0))
  70 | #define AB_RIGHT_TRANSFER       ((ab_rights_t)(1U << 1))
  71 | #define AB_RIGHT_READ           ((ab_rights_t)(1U << 2))
  72 | #define AB_RIGHT_WRITE          ((ab_rights_t)(1U << 3))
  73 | #define AB_RIGHT_EXECUTE        ((ab_rights_t)(1U << 4))
  74 | #define AB_RIGHT_MAP            ((ab_rights_t)(1U << 5))
  75 | #define AB_RIGHT_GET_PROPERTY   ((ab_rights_t)(1U << 6))
  76 | #define AB_RIGHT_SET_PROPERTY   ((ab_rights_t)(1U << 7))
  77 | #define AB_RIGHT_SIGNAL         ((ab_rights_t)(1U << 8))
  78 | #define AB_RIGHT_SIGNAL_PEER    ((ab_rights_t)(1U << 9))
  79 | #define AB_RIGHT_WAIT           ((ab_rights_t)(1U << 10))
  80 | #define AB_RIGHT_INSPECT        ((ab_rights_t)(1U << 11))
  81 | #define AB_RIGHT_MANAGE_PROCESS ((ab_rights_t)(1U << 12))
  82 | #define AB_RIGHT_MANAGE_THREAD  ((ab_rights_t)(1U << 13))
  83 | #define AB_RIGHT_SAME_RIGHTS    ((ab_rights_t)(1U << 31))
  84 | 
  85 | #define AB_RIGHTS_BASIC            (AB_RIGHT_TRANSFER | AB_RIGHT_DUPLICATE | AB_RIGHT_WAIT | AB_RIGHT_INSPECT)
  86 | #define AB_RIGHTS_IO               (AB_RIGHT_READ | AB_RIGHT_WRITE)
  87 | #define AB_RIGHTS_DEFAULT_LOG      (AB_RIGHTS_BASIC | AB_RIGHT_WRITE)
  88 | #define AB_RIGHTS_DEFAULT_EVENT    (AB_RIGHTS_BASIC | AB_RIGHT_SIGNAL)
  89 | #define AB_RIGHTS_DEFAULT_EVENTPAIR (AB_RIGHTS_BASIC | AB_RIGHT_SIGNAL | AB_RIGHT_SIGNAL_PEER)
  90 | 
       //// Object Signals Bitmask (AB_SIGNAL_*):
       ////   - Bits 0..3   : Kernel-defined lifecycle & readiness signals
       ////                   (READABLE, WRITABLE, PEER_CLOSED, SIGNALED).
       ////   - Bits 24..27 : Application-defined signals (AB_USER_SIGNAL_0..3).
       //// As seen in Chapter 11, EL0 may only mutate SIGNALED and USER_SIGNAL_ALL.
  91 | /* Object Signals Bitmask (zircon_org/system/public/zircon/types.h) */
  92 | #define AB_SIGNAL_NONE          ((ab_signals_t)0U)
  93 | #define AB_SIGNAL_READABLE      ((ab_signals_t)(1U << 0))
  94 | #define AB_SIGNAL_WRITABLE      ((ab_signals_t)(1U << 1))
  95 | #define AB_SIGNAL_PEER_CLOSED   ((ab_signals_t)(1U << 2))
  96 | #define AB_SIGNAL_SIGNALED      ((ab_signals_t)(1U << 3))
  97 | #define AB_USER_SIGNAL_0        ((ab_signals_t)(1U << 24))
  98 | #define AB_USER_SIGNAL_1        ((ab_signals_t)(1U << 25))
  99 | #define AB_USER_SIGNAL_2        ((ab_signals_t)(1U << 26))
 100 | #define AB_USER_SIGNAL_3        ((ab_signals_t)(1U << 27))
 101 | #define AB_USER_SIGNAL_ALL \
 102 |     (AB_USER_SIGNAL_0 | AB_USER_SIGNAL_1 | AB_USER_SIGNAL_2 | AB_USER_SIGNAL_3)
 103 | 
       //// ab_wait_observer_t & ab_state_tracker_t:
       //// Every kernel object embeds an ab_state_tracker_t holding its active
       //// 32-bit signal mask and a doubly-linked list of registered observers.
       ////   - When is_async_port == 0 (synchronous sys_object_wait_one), the
       ////     observer lives on the waiting thread's kernel stack and wakes
       ////     observer->event.
       ////   - When is_async_port == 1 (asynchronous sys_object_wait_async), the
       ////     observer is heap-allocated, holds a reference to port_obj, posts an
       ////     AB_PKT_TYPE_SIGNAL_ONE packet tagged with 'key', and self-unlinks!
 104 | /* WaitStateObserver node registered on an object's StateTracker */
 105 | struct ab_object;
 106 | typedef struct ab_wait_observer {
 107 |     struct ab_wait_observer *prev;
 108 |     struct ab_wait_observer *next;
 109 |     ab_signals_t             watched_signals;
 110 |     ab_signals_t             wakeup_reasons;
 111 |     event_t                  event;
 112 |     int                      is_async_port;
 113 |     struct ab_object        *port_obj;
 114 |     uint64_t                 key;
 115 | } ab_wait_observer_t;
 116 | 
 117 | /* StateTracker embedded in every Dispatcher */
 118 | typedef struct ab_state_tracker {
 119 |     spin_lock_t         lock;
 120 |     ab_signals_t        signals;
 121 |     ab_wait_observer_t *head;
 122 |     ab_wait_observer_t *tail;
 123 | } ab_state_tracker_t;
 124 | 
       //// ab_handle_basic_info_t (returned to EL0 by sys_object_get_info) and
       //// the polymorphic lifecycle hook signature ab_obj_hook_t.
 125 | /* Object info returned by ab_object_get_info */
 126 | typedef struct {
 127 |     ab_koid_t      koid;
 128 |     ab_koid_t      related_koid;
 129 |     ab_obj_type_t  type;
 130 |     ab_rights_t    rights;
 131 |     uint32_t       handle_count;
 132 | } ab_handle_basic_info_t;
 133 | 
 134 | struct ab_object;
 135 | typedef void (*ab_obj_hook_t)(struct ab_object *obj);
 136 | 
       //// POLYMORPHIC BASE DISPATCHER (ab_object_t):
       //// C equivalent of Zircon's C++ fbl::RefCounted + Dispatcher base class.
       //// Placed at byte offset 0 of every concrete dispatcher so pointers cast
       //// safely between (ab_object_t *) and derived types!
       ////
       //// DUAL REFERENCE COUNTING INVARIANT:
       ////   - handle_count : Number of open user-space handles across all
       ////                    processes (plus in-flight channel handle slots).
       ////                    Transitioning 1 -> 0 fires on_zero_handles(obj)
       ////                    (e.g., notifying an EventPair/Channel peer of closure).
       ////   - ref_count    : Total kernel pointers (handle table entries + active
       ////                    syscall lookups + VMAR/Job links + peer pointers).
       ////                    Transitioning 1 -> 0 fires destroy(obj) -> kfree(obj).
 137 | /* Base Kernel Object (models Zircon's Dispatcher class) */
 138 | typedef struct ab_object {
 139 |     ab_koid_t          koid;
 140 |     ab_koid_t          related_koid;
 141 |     ab_obj_type_t      type;
 142 |     volatile int32_t   ref_count;
 143 |     volatile int32_t   handle_count;
 144 |     ab_state_tracker_t state_tracker;
 145 |     ab_obj_hook_t      on_zero_handles;
 146 |     ab_obj_hook_t      destroy;
 147 | } ab_object_t;
 148 | 
       //// Per-process handle table entry (ab_handle_entry_t):
       //// Pairs the target ab_object_t* and ab_rights_t with a monotonically
       //// incremented 32-bit 'generation' counter (encoded into bits [23:8] of
       //// ab_handle_t) so closing a slot and reallocating it immediately invalidates
       //// any stale handle integer still held by buggy EL0 code (ABA defense!).
 149 | #define MAX_HANDLES_PER_PROCESS 64
 150 | 
 151 | typedef struct {
 152 |     ab_object_t *obj;
 153 |     ab_rights_t  rights;
 154 |     uint32_t     generation;
 155 | } ab_handle_entry_t;
 156 | 
       //// Resource bounds and VMO/VMAR default rights:
       ////   - VMO_MAX_PAGES = 128 pages (512 KB max per VMO in Lesson 20).
       ////   - MAX_VMAR_MAPPINGS = 16 regions per process.
       ////   - MAX_THREADS_PER_PROCESS = 8 threads, MAX_PROCS_PER_JOB = 16 procs.
 157 | #define VMO_MAX_PAGES           128U
 158 | #define MAX_VMAR_MAPPINGS       16U
 159 | #define MAX_THREADS_PER_PROCESS 8U
 160 | #define MAX_PROCS_PER_JOB       16U
 161 | 
 162 | #define AB_VM_PERM_READ         (1U << 0)
 163 | #define AB_VM_PERM_WRITE        (1U << 1)
 164 | #define AB_VM_PERM_EXECUTE      (1U << 2)
 165 | 
 166 | #define AB_RIGHTS_DEFAULT_VMO (AB_RIGHT_EXECUTE | \
 167 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_MAP | AB_RIGHT_SET_PROPERTY))
 168 | #define AB_RIGHTS_DEFAULT_VMAR \
 169 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_MAP | AB_RIGHT_EXECUTE)
 170 | 
 171 | struct ab_thread_dispatcher;
 172 | struct ab_job_dispatcher;
 173 | 
       //// ab_vmo_dispatcher_t (Virtual Memory Object):
       //// Holds a per-VMO spinlock, rounded byte/page size, is_physical flag
       //// (1 = Device-nGnRnE MMIO; 0 = Normal WBWA RAM), and sparse physical
       //// frame array paddrs[128] where 0 indicates an uncommitted lazy page.
 174 | typedef struct ab_vmo_dispatcher {
 175 |     ab_object_t base;
 176 |     char        name[32];
 177 |     spin_lock_t lock;
 178 |     size_t      size_bytes;
 179 |     uint32_t    num_pages;
 180 |     int         is_physical;
 181 |     paddr_t     paddrs[VMO_MAX_PAGES];
 182 | } ab_vmo_dispatcher_t;
 183 | 
       //// ab_vmar_dispatcher_t (Virtual Memory Address Region):
       //// Capability wrapper governing a target process's user address window
       //// [base_va, base_va + size) (0x00001000..0x80000000 in Lesson 20).
 184 | typedef struct ab_vmar_dispatcher {
 185 |     ab_object_t          base;
 186 |     struct ab_process   *proc;
 187 |     vaddr_t              base_va;
 188 |     size_t               size;
 189 | } ab_vmar_dispatcher_t;
 190 | 
       //// ab_vmar_mapping_t:
       //// Records an active VMO-to-VMAR mapping inside ab_process_t.mappings[16],
       //// holding a counted reference on 'vmo' plus vmo_offset and AB_VM_PERM_*
       //// flags consulted by the lazy demand-paging handler.
 191 | typedef struct {
 192 |     int                  active;
 193 |     vaddr_t              base_va;
 194 |     size_t               size;
 195 |     size_t               vmo_offset;
 196 |     uint32_t             vm_perms;
 197 |     ab_vmo_dispatcher_t *vmo;
 198 | } ab_vmar_mapping_t;
 199 | 
       //// ab_process_t (ProcessDispatcher):
       //// Unifies a process's hardware VmAspace, capability handle table
       //// (handles[64] under handle_lock), virtual mappings (mappings[16] under
       //// vmar_lock), child threads[8], parent Job link, EL0 stack tracking, and
       //// exit status.
 200 | typedef struct ab_process {
 201 |     ab_object_t                  base;
 202 |     char                         name[32];
 203 |     struct ab_job_dispatcher    *parent_job;
 204 |     vm_aspace_t                 *aspace;
 205 |     spin_lock_t                  handle_lock;
 206 |     spin_lock_t                  vmar_lock;
 207 |     ab_handle_entry_t            handles[MAX_HANDLES_PER_PROCESS];
 208 |     ab_vmar_mapping_t            mappings[MAX_VMAR_MAPPINGS];
 209 |     struct ab_thread_dispatcher *threads[MAX_THREADS_PER_PROCESS];
 210 |     uint32_t                     thread_count;
 211 |     vaddr_t                      stack_va;
 212 |     size_t                       stack_size;
 213 |     ab_koid_t                    stack_vmo_koid;
 214 |     vaddr_t                      next_map_va;
 215 |     int                          exit_code;
 216 |     int                          exited;
 217 | } ab_process_t;
 218 | 
       //// Lightweight Log and Event dispatchers embedding ab_object_t at offset 0.
 219 | typedef struct {
 220 |     ab_object_t base;
 221 |     uint32_t    flags;
 222 | } ab_log_dispatcher_t;
 223 | 
 224 | typedef struct {
 225 |     ab_object_t base;
 226 |     uint32_t    options;
 227 | } ab_event_dispatcher_t;
 228 | 
       //// ab_eventpair_dispatcher_t:
       //// Uses a dedicated peer_lock protecting its raw 'peer' pointer so
       //// concurrent peer signaling and peer closure never race or deadlock
       //// against per-object StateTracker locks.
 229 | typedef struct ab_eventpair_dispatcher {
 230 |     ab_object_t                     base;
 231 |     spin_lock_t                     peer_lock;
 232 |     struct ab_eventpair_dispatcher *peer;
 233 | } ab_eventpair_dispatcher_t;
 234 | 
       //// Base object allocator, atomic KOID generator, and reference-counting
       //// primitives implemented in kernel/core/object.c.
 235 | ab_koid_t    ab_koid_alloc(void);
 236 | ab_object_t *ab_object_alloc(size_t struct_size, ab_obj_type_t type,
 237 |                              ab_signals_t initial_signals,
 238 |                              ab_obj_hook_t on_zero_handles, ab_obj_hook_t destroy);
 239 | void         ab_object_add_ref(ab_object_t *obj);
 240 | void         ab_object_release(ab_object_t *obj);
 241 | const char  *ab_obj_type_name(ab_obj_type_t type);
 242 | 
       //// StateTracker signal mutation, query, and synchronous timed wait API.
 243 | /* StateTracker API */
 244 | void         state_tracker_update_signals(ab_object_t *obj,
 245 |                                           ab_signals_t clear_mask,
 246 |                                           ab_signals_t set_mask);
 247 | ab_signals_t state_tracker_get_signals(ab_object_t *obj);
 248 | ab_status_t  state_tracker_wait_one(ab_object_t *obj,
 249 |                                     ab_signals_t watched_signals,
 250 |                                     ab_time_t deadline_ns,
 251 |                                     ab_signals_t *observed_out);
 252 | 
       //// Process creation and per-process capability handle table operations
       //// (alloc, type+rights lookup, close, duplicate, and replace).
 253 | /* Process & Handle Table API */
 254 | ab_process_t *ab_process_create_kernel(const char *name, uint16_t asid);
 255 | ab_process_t *get_current_process(void);
 256 | 
 257 | ab_status_t handle_alloc(ab_process_t *proc, ab_object_t *obj, ab_rights_t rights,
 258 |                          ab_handle_t *out_handle);
 259 | ab_status_t handle_lookup(ab_process_t *proc, ab_handle_t h, ab_obj_type_t expected_type,
 260 |                           ab_rights_t required_rights, ab_object_t **out_obj,
 261 |                           ab_rights_t *out_rights);
 262 | ab_status_t handle_close(ab_process_t *proc, ab_handle_t h);
 263 | ab_status_t handle_duplicate(ab_process_t *proc, ab_handle_t h, ab_rights_t new_rights,
 264 |                              ab_handle_t *out_new_handle);
 265 | ab_status_t handle_replace(ab_process_t *proc, ab_handle_t h, ab_rights_t new_rights,
 266 |                            ab_handle_t *out_new_handle);
 267 | 
       //// Factory constructors and peer-signaling API for Log, Event, and
       //// EventPair dispatchers.
 268 | ab_status_t ab_log_dispatcher_create(uint32_t flags, ab_object_t **out_obj);
 269 | ab_status_t ab_event_dispatcher_create(uint32_t options, ab_object_t **out_obj);
 270 | ab_status_t ab_eventpair_dispatcher_create(ab_object_t **out_obj0,
 271 |                                            ab_object_t **out_obj1);
 272 | ab_status_t ab_eventpair_signal_peer(ab_object_t *obj,
 273 |                                      ab_signals_t clear_mask,
 274 |                                      ab_signals_t set_mask);
 275 | 
 276 | 
       //// --------------------------------------------------------------------------
       //// PORT DISPATCHER & ASYNCHRONOUS EVENT MULTIPLEXING (Lines 277-318)
       //// --------------------------------------------------------------------------
       //// Default capability rights and packet type tags (USER = 0, SIGNAL_ONE = 1)
       //// for Zircon-style asynchronous event ports.
 277 | /* =========================================================================
 278 |  * Lesson 09: PortDispatcher & Asynchronous Wait (zx_port_packet_t)
 279 |  * References:
 280 |  *   - zircon_org/kernel/object/include/object/port_dispatcher.h
 281 |  *   - zircon_org/system/public/zircon/syscalls/port.h
 282 |  * ========================================================================= */
 283 | #define AB_RIGHTS_DEFAULT_PORT \
 284 |     (AB_RIGHT_TRANSFER | AB_RIGHT_DUPLICATE | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_INSPECT)
 285 | 
 286 | #define AB_PKT_TYPE_USER        0U
 287 | #define AB_PKT_TYPE_SIGNAL_ONE  1U
 288 | 
       //// ab_port_packet_t (48 bytes with alignment): Fixed-size notification
       //// record dequeued by sys_port_wait(), carrying caller-assigned 'key',
       //// packet 'type', status, watched 'trigger' mask, 'observed' signal
       //// snapshot, monotonic nanosecond 'timestamp', and custom 'user_val'.
 289 | typedef struct {
 290 |     uint64_t     key;
 291 |     uint32_t     type;
 292 |     ab_status_t  status;
 293 |     ab_signals_t trigger;
 294 |     ab_signals_t observed;
 295 |     uint64_t     timestamp;
 296 |     uint64_t     user_val;
 297 | } ab_port_packet_t;
 298 | 
 299 | #define PORT_QUEUE_CAPACITY 32
 300 | 
       //// ab_port_dispatcher_t: Embeds a 32-slot circular packet ring buffer
       //// (packets[], head, tail, count) protected by 'lock' plus 'readable_event'
       //// to wake server event-loop threads blocked in ab_port_wait().
 301 | typedef struct ab_port_dispatcher {
 302 |     ab_object_t      base;
 303 |     uint32_t         options;
 304 |     spin_lock_t      lock;
 305 |     ab_port_packet_t packets[PORT_QUEUE_CAPACITY];
 306 |     uint32_t         head;
 307 |     uint32_t         tail;
 308 |     uint32_t         count;
 309 |     event_t           readable_event;
 310 | } ab_port_dispatcher_t;
 311 | 
       //// Port creation, packet queueing, timed dequeue, and one-shot
       //// asynchronous StateTracker registration (state_tracker_wait_async).
 312 | ab_status_t ab_port_dispatcher_create(uint32_t options, ab_object_t **out_obj);
 313 | ab_status_t ab_port_queue(ab_object_t *port_obj, const ab_port_packet_t *pkt);
 314 | ab_status_t ab_port_wait(ab_object_t *port_obj, ab_time_t deadline_ns,
 315 |                          ab_port_packet_t *out_pkt);
 316 | ab_status_t state_tracker_wait_async(ab_object_t *obj, ab_object_t *port_obj,
 317 |                                      uint64_t key, ab_signals_t watched_signals);
 318 | 
       //// --------------------------------------------------------------------------
       //// CHANNEL DISPATCHER & CAPABILITY IPC (Lines 319-381)
       //// --------------------------------------------------------------------------
       //// Default channel rights (including TRANSFER, DUPLICATE, READ, WRITE,
       //// SIGNAL, and SIGNAL_PEER) and per-message payload caps (256B, 8 handles).
 319 | /* =========================================================================
 320 |  * Lesson 10: ChannelDispatcher & Capability Transfer (MessagePacket)
 321 |  * References:
 322 |  *   - zircon_org/kernel/object/include/object/channel_dispatcher.h
 323 |  *   - zircon_org/kernel/object/include/object/message_packet.h
 324 |  * ========================================================================= */
 325 | #define AB_RIGHTS_DEFAULT_CHANNEL (AB_RIGHT_DUPLICATE | \
 326 |     (AB_RIGHT_TRANSFER | AB_RIGHT_WAIT | AB_RIGHT_INSPECT | \
 327 |      AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_SIGNAL | AB_RIGHT_SIGNAL_PEER))
 328 | 
 329 | #define CHANNEL_MAX_MSG_BYTES   256U
 330 | #define CHANNEL_MAX_MSG_HANDLES 8U
 331 | 
       //// ab_message_packet_t: Heap-allocated FIFO node representing an in-flight
       //// Channel datagram. Crucially, objs[0..7] hold raw ab_object_t* kernel
       //// pointers (with both ref_count and handle_count kept elevated!) while
       //// capabilities are in transit between sender and receiver handle tables.
 332 | typedef struct ab_message_packet {
 333 |     struct ab_message_packet *next;
 334 |     uint32_t                  num_bytes;
 335 |     uint32_t                  num_handles;
 336 |     uint8_t                   data[CHANNEL_MAX_MSG_BYTES];
 337 |     ab_object_t              *objs[CHANNEL_MAX_MSG_HANDLES];
 338 |     ab_rights_t               rights[CHANNEL_MAX_MSG_HANDLES];
 339 | } ab_message_packet_t;
 340 | 
       //// ab_channel_dispatcher_t: One endpoint of an entangled bidirectional
       //// channel pair, maintaining a singly-linked FIFO [msg_head .. msg_tail]
       //// of unread incoming packets.
 341 | typedef struct ab_channel_dispatcher {
 342 |     ab_object_t                   base;
 343 |     spin_lock_t                   lock;
 344 |     struct ab_channel_dispatcher *peer;
 345 |     ab_message_packet_t          *msg_head;
 346 |     ab_message_packet_t          *msg_tail;
 347 |     uint32_t                      msg_count;
 348 | } ab_channel_dispatcher_t;
 349 | 
       //// User-space syscall argument packing structs for SYS_CHANNEL_WRITE
       //// (ab_channel_write_args_t) and SYS_CHANNEL_READ (ab_channel_read_args_t),
       //// copied atomically via copy_from_user() in syscalls.c.
 350 | typedef struct {
 351 |     const void        *bytes;
 352 |     uint32_t           num_bytes;
 353 |     const ab_handle_t *handles;
 354 |     uint32_t           num_handles;
 355 | } ab_channel_write_args_t;
 356 | 
 357 | typedef struct {
 358 |     void        *bytes;
 359 |     ab_handle_t *handles;
 360 |     uint32_t     max_bytes;
 361 |     uint32_t     max_handles;
 362 |     uint32_t    *actual_bytes;
 363 |     uint32_t    *actual_handles;
 364 | } ab_channel_read_args_t;
 365 | 
       //// Two-phase capability transfer helpers (removing a handle with
       //// AB_RIGHT_TRANSFER from the sender — while forbidding self-transfer of
       //// the channel endpoint being written to! — and installing it into the
       //// receiver) plus Channel create/write/read entry points.
 366 | ab_status_t handle_take_for_transfer(ab_process_t *proc, ab_handle_t h,
 367 |                                      ab_object_t *channel_self,
 368 |                                      ab_object_t **out_obj, ab_rights_t *out_rights);
 369 | ab_status_t handle_install_transferred(ab_process_t *proc, ab_object_t *obj,
 370 |                                        ab_rights_t rights, ab_handle_t *out_handle);
 371 | 
 372 | ab_status_t ab_channel_dispatcher_create(ab_object_t **out_obj0,
 373 |                                          ab_object_t **out_obj1);
 374 | ab_status_t ab_channel_write(ab_process_t *sender_proc, ab_object_t *ch_obj,
 375 |                              const void *bytes, uint32_t num_bytes,
 376 |                              const ab_handle_t *handles, uint32_t num_handles);
 377 | ab_status_t ab_channel_read(ab_process_t *receiver_proc, ab_object_t *ch_obj,
 378 |                             void *bytes, ab_handle_t *handles,
 379 |                             uint32_t max_bytes, uint32_t max_handles,
 380 |                             uint32_t *actual_bytes, uint32_t *actual_handles);
 381 | 
 382 | 
       //// --------------------------------------------------------------------------
       //// VMO & VMAR MAPPING CONTRACTS (Lines 383-408)
       //// --------------------------------------------------------------------------
       //// ab_vmar_map_args_t packs the 5 auxiliary parameters of SYS_VMAR_MAP.
 383 | /* Lesson 11: VMO & VMAR APIs */
 384 | typedef struct {
 385 |     ab_handle_t vmo_handle;
 386 |     uint64_t    vmo_offset;
 387 |     uint64_t    len;
 388 |     uint32_t    map_flags;
 389 |     vaddr_t    *mapped_va_out;
 390 | } ab_vmar_map_args_t;
 391 | 
       //// VMO creation, lazy page lookup/allocation, and kernel-side read/write
       //// primitives used by both syscalls.c and bootfs initialization in main.c.
 392 | ab_status_t ab_vmo_dispatcher_create(size_t size_bytes, uint32_t options,
 393 |                                      ab_object_t **out_obj);
 394 | ab_status_t ab_vmo_get_or_alloc_page(ab_vmo_dispatcher_t *vmo, uint32_t page_idx,
 395 |                                      int alloc_if_missing, paddr_t *out_pa);
 396 | ab_status_t ab_vmo_read(ab_object_t *vmo_obj, void *buf, uint64_t offset, size_t len);
 397 | ab_status_t ab_vmo_write(ab_object_t *vmo_obj, const void *buf, uint64_t offset, size_t len);
 398 | 
       //// VMAR creation, region map/unmap, and ab_process_handle_page_fault() —
       //// the unified demand-paging resolver shared by hardware EL0 aborts
       //// (exceptions_c.c) and software user-pointer validation (syscalls.c).
 399 | ab_status_t ab_vmar_dispatcher_create(ab_process_t *proc, ab_object_t **out_obj);
 400 | ab_status_t ab_vmar_map(ab_process_t *proc, ab_object_t *vmar_obj,
 401 |                         size_t vmar_offset, ab_object_t *vmo_obj,
 402 |                         uint64_t vmo_offset, size_t len, uint32_t vm_perms,
 403 |                         vaddr_t *out_va);
 404 | ab_status_t ab_vmar_unmap(ab_process_t *proc, ab_object_t *vmar_obj,
 405 |                           vaddr_t addr, size_t len);
 406 | int         ab_process_handle_page_fault(ab_process_t *proc, vaddr_t fault_va,
 407 |                                          int is_write, int is_exec);
 408 | 
 409 | 
       //// --------------------------------------------------------------------------
       //// ZIRCON TASK HIERARCHY: JOB, PROCESS & THREAD (Lines 410-469)
       //// --------------------------------------------------------------------------
       //// AB_SIGNAL_TASK_TERMINATED aliases AB_SIGNAL_SIGNALED so supervisors
       //// wait for child exit via sys_object_wait_one(), followed by default
       //// Job, Process, and Thread capability right bundles.
 410 | /* =========================================================================
 411 |  * Lesson 12: Tasks (JobDispatcher, ProcessDispatcher, ThreadDispatcher)
 412 |  * References:
 413 |  *   - zircon_org/kernel/object/include/object/job_dispatcher.h
 414 |  *   - zircon_org/kernel/object/include/object/process_dispatcher.h
 415 |  *   - zircon_org/kernel/object/include/object/thread_dispatcher.h
 416 |  * ========================================================================= */
 417 | #define AB_SIGNAL_TASK_TERMINATED AB_SIGNAL_SIGNALED
 418 | 
 419 | #define AB_RIGHTS_DEFAULT_JOB \
 420 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE | \
 421 |      AB_RIGHT_MANAGE_PROCESS | AB_RIGHT_MANAGE_THREAD)
 422 | #define AB_RIGHTS_DEFAULT_PROCESS \
 423 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_MANAGE_THREAD)
 424 | #define AB_RIGHTS_DEFAULT_THREAD \
 425 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE)
 426 | 
       //// ab_job_dispatcher_t: Policy and resource container grouping up to
       //// MAX_PROCS_PER_JOB (16) child processes under a spinlock.
 427 | typedef struct ab_job_dispatcher {
 428 |     ab_object_t   base;
 429 |     char          name[32];
 430 |     spin_lock_t   lock;
 431 |     uint32_t      child_count;
 432 |     ab_process_t *procs[MAX_PROCS_PER_JOB];
 433 | } ab_job_dispatcher_t;
 434 | 
       //// ab_thread_dispatcher_t: Zircon capability wrapper around an underlying
       //// LK thread_t* ('kthread'), storing the initial EL0 entry PC, user SP,
       //// and two startup register arguments (arg0 = handle, arg1 = value).
 435 | typedef struct ab_thread_dispatcher {
 436 |     ab_object_t        base;
 437 |     char               name[32];
 438 |     ab_process_t      *proc;
 439 |     thread_t          *kthread;
 440 |     uintptr_t          user_entry;
 441 |     uintptr_t          user_sp;
 442 |     uint64_t           arg0;
 443 |     uint64_t           arg1;
 444 | } ab_thread_dispatcher_t;
 445 | 
       //// Packed syscall descriptors for SYS_PROCESS_CREATE and SYS_PROCESS_START.
 446 | typedef struct {
 447 |     const char  *name;
 448 |     size_t       name_len;
 449 |     ab_handle_t *proc_handle_out;
 450 |     ab_handle_t *vmar_handle_out;
 451 | } ab_process_create_args_t;
 452 | 
 453 | typedef struct {
 454 |     ab_handle_t thread_handle;
 455 |     uintptr_t   entry_pc;
 456 |     uintptr_t   user_sp;
 457 |     ab_handle_t arg_handle;
 458 |     uint64_t    arg2;
 459 | } ab_process_start_args_t;
 460 | 
       //// Kernel constructors for JobDispatcher and ThreadDispatcher, plus
       //// ab_process_start_user() which transfers arg_handle into the child
       //// process and transitions its initial thread into EL0 via uspace_entry.S.
 461 | ab_status_t ab_job_dispatcher_create(ab_object_t **out_obj);
 462 | ab_status_t ab_thread_dispatcher_create(ab_process_t *proc, const char *name,
 463 |                                         ab_object_t **out_obj);
 464 | ab_status_t ab_process_start_user(ab_process_t *caller_proc,
 465 |                                   ab_process_t *target_proc,
 466 |                                   ab_thread_dispatcher_t *uthr,
 467 |                                   uintptr_t entry_pc, uintptr_t user_sp,
 468 |                                   ab_handle_t arg_handle, uint64_t arg2);
 469 | 
 470 | 
       //// --------------------------------------------------------------------------
       //// USER-SPACE DRIVER DDK: RESOURCE, DMA VMO & INTERRUPT (Lines 471-517)
       //// --------------------------------------------------------------------------
       //// Default capability rights for hardware Resource and Interrupt objects.
 471 | /* =========================================================================
 472 |  * Lesson 13: Driver Capabilities (ResourceDispatcher, Physical/DMA VMO,
 473 |  *            and InterruptDispatcher)
 474 |  * References:
 475 |  *   - zircon_org/kernel/object/include/object/resource_dispatcher.h
 476 |  *   - zircon_org/kernel/object/include/object/interrupt_dispatcher.h
 477 |  *   - zircon_org/kernel/syscalls/ddk.cpp
 478 |  * ========================================================================= */
 479 | #define AB_RIGHTS_DEFAULT_RESOURCE \
 480 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_EXECUTE)
 481 | #define AB_RIGHTS_DEFAULT_INTERRUPT \
 482 |     (AB_RIGHTS_BASIC | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_SIGNAL)
 483 | 
       //// ab_resource_dispatcher_t & ab_interrupt_dispatcher_t:
       ////   - ResourceDispatcher acts as an unforgeable capability key gating
       ////     MMIO, contiguous DMA, and IRQ syscalls.
       ////   - InterruptDispatcher binds GIC SPI 'irq_num' to 'irq_event' and
       ////     tracks 'trigger_count' across hardware interrupts.
 484 | typedef struct ab_resource_dispatcher {
 485 |     ab_object_t base;
 486 |     uint32_t    kind;
 487 | } ab_resource_dispatcher_t;
 488 | 
 489 | typedef struct ab_interrupt_dispatcher {
 490 |     ab_object_t base;
 491 |     uint32_t    irq_num;
 492 |     event_t     irq_event;
 493 |     uint64_t    trigger_count;
 494 | } ab_interrupt_dispatcher_t;
 495 | 
       //// Packed syscall descriptors for SYS_VMO_CREATE_PHYSICAL (MMIO) and
       //// SYS_VMO_CREATE_CONTIG (physically contiguous virtio DMA rings/buffers).
 496 | typedef struct {
 497 |     paddr_t      paddr;
 498 |     size_t       size;
 499 |     ab_handle_t *vmo_out;
 500 | } ab_vmo_create_phys_args_t;
 501 | 
 502 | typedef struct {
 503 |     size_t       size;
 504 |     ab_handle_t *vmo_out;
 505 |     paddr_t     *paddr_out;
 506 | } ab_vmo_create_contig_args_t;
 507 | 
       //// DDK object constructors, user-space IRQ wait/ack routines, and the
       //// top-half IRQ hook ab_interrupt_dispatch_hw_irq() called by exceptions_c.c.
 508 | ab_status_t ab_resource_dispatcher_create(uint32_t kind, ab_object_t **out_obj);
 509 | ab_status_t ab_vmo_create_physical(paddr_t paddr, size_t size_bytes,
 510 |                                    ab_object_t **out_obj);
 511 | ab_status_t ab_vmo_create_contiguous(size_t size_bytes, ab_object_t **out_obj,
 512 |                                      paddr_t *out_paddr);
 513 | ab_status_t ab_interrupt_dispatcher_create(uint32_t irq_num, ab_object_t **out_obj);
 514 | ab_status_t ab_interrupt_wait(ab_object_t *intr_obj);
 515 | ab_status_t ab_interrupt_ack(ab_object_t *intr_obj);
 516 | int         ab_interrupt_dispatch_hw_irq(uint32_t irq_num);
 517 | 
       //// --------------------------------------------------------------------------
       //// TASK & VM INTROSPECTION ('ps' & 'vmaps') (Lines 518-590)
       //// --------------------------------------------------------------------------
       //// Diagnostic buffer caps for the user-space 'ps' and 'vmaps' utilities.
 518 | /* =========================================================================
 519 |  * Lesson 17: Zircon-Style Task & Address Space Introspection ('ps' & 'vmaps')
 520 |  * References:
 521 |  *   - zircon_org/kernel/object/diagnostics.cpp (DumpProcessList, GetVmAspaceMaps)
 522 |  *   - zircon_org/system/public/zircon/syscalls/object.h (zx_info_maps_t, zx_info_task_stats_t)
 523 |  *   - zircon_org/system/uapp/psutils/ps.c & vmaps.c
 524 |  * ========================================================================= */
 525 | #define AB_MAX_PS_ENTRIES 24U
 526 | #define AB_MAX_VMAPS      16U
 527 | 
       //// ab_task_info_entry_t (152 bytes): Single flattened row of the Job ->
       //// Process -> Thread tree exported by SYS_JOB_GET_PS_INFO, including MMU
       //// ASID, Proportional/Private/Shared memory accounting (PSS splits shared
       //// VMO pages such as bootfs.tar proportionally across mapping processes!),
       //// and per-type capability handle counts.
 528 | /* Task Tree & Handle Table Diagnostics Entry (used by 'ps') */
 529 | typedef struct {
 530 |     char      type;          /* 'j' = Job, 'p' = Process, 't' = Thread */
 531 |     uint8_t   depth;         /* Hierarchy indentation depth (0=job, 1=proc, 2=thread) */
 532 |     uint16_t  asid;          /* Hardware MMU ASID (for processes) */
 533 |     ab_koid_t koid;          /* Kernel Object ID */
 534 |     ab_koid_t parent_koid;   /* Parent Job/Process KOID */
 535 |     char      name[32];      /* Task name */
 536 |     char      state[12];     /* Thread/Process state ("running", "blocked", "ready", etc.) */
 537 |     size_t    pss_bytes;     /* Proportional Set Size (private + scaled shared bytes) */
 538 |     size_t    private_bytes; /* Private committed memory in bytes */
 539 |     size_t    shared_bytes;  /* Shared committed memory in bytes */
 540 |     uint32_t  total_handles; /* Total open handles in process */
 541 |     uint32_t  handle_counts[AB_OBJ_TYPE_COUNT]; /* Per-type handle count (#jb,#pr,#th,#vo,#vm,#ch,#ev,#po,#ir,#rs,#lg) */
 542 | } ab_task_info_entry_t;
 543 | 
       //// Node type constants (ASPACE, VMAR, MAPPING) and mapping detail payload
       //// (ab_info_maps_mapping_t) modeling Zircon's zx_info_maps_t hierarchy.
 544 | /* VM Mapping Hierarchy Types (models Zircon's zx_info_maps_type_t) */
 545 | typedef uint32_t ab_info_maps_type_t;
 546 | #define AB_INFO_MAPS_TYPE_NONE    0U
 547 | #define AB_INFO_MAPS_TYPE_ASPACE  1U
 548 | #define AB_INFO_MAPS_TYPE_VMAR    2U
 549 | #define AB_INFO_MAPS_TYPE_MAPPING 3U
 550 | 
 551 | /* Describes a VM mapping (models Zircon's zx_info_maps_mapping_t) */
 552 | typedef struct {
 553 |     uint32_t  mmu_flags;       /* Bitwise OR of AB_VM_PERM_{READ,WRITE,EXECUTE} */
 554 |     ab_koid_t vmo_koid;        /* KOID of the backing VMO */
 555 |     uint64_t  vmo_offset;      /* Offset into the above VMO */
 556 |     size_t    committed_pages; /* Number of PAGE_SIZE pages backed by physical RAM/MMIO */
 557 |     paddr_t   first_paddr;     /* Physical address of first mapped page (0 if uncommitted) */
 558 | } ab_info_maps_mapping_t;
 559 | 
       //// ab_info_maps_t (104 bytes) & ab_process_get_maps_args_t: Wire structures
       //// for SYS_PROCESS_GET_MAPS (target_proc_koid == 0 inspects the caller).
 560 | /* Describes a node in the aspace/vmar/mapping hierarchy (models Zircon's zx_info_maps_t) */
 561 | typedef struct {
 562 |     char                   name[32];
 563 |     vaddr_t                base;
 564 |     size_t                 size;
 565 |     size_t                 depth;
 566 |     ab_info_maps_type_t    type;
 567 |     ab_info_maps_mapping_t mapping;
 568 | } ab_info_maps_t;
 569 | 
 570 | typedef struct {
 571 |     ab_koid_t        target_proc_koid; /* 0 = calling process */
 572 |     ab_info_maps_t  *maps_out;
 573 |     uint32_t         max_maps;
 574 |     uint32_t        *actual_out;
 575 | } ab_process_get_maps_args_t;
 576 | 
       //// Job membership helpers, object naming API, and the kernel diagnostic
       //// walkers backing 'ps' (ab_job_get_ps_info) and 'vmaps'
       //// (ab_job_get_process_maps).
 577 | void        ab_job_add_process(ab_job_dispatcher_t *job, ab_process_t *proc);
 578 | void        ab_job_remove_process(ab_process_t *proc);
 579 | ab_status_t ab_object_set_name(ab_object_t *obj, const char *name, size_t len);
 580 | ab_status_t ab_job_get_ps_info(ab_object_t *job_obj, ab_task_info_entry_t *out_entries,
 581 |                                uint32_t max_entries, uint32_t *actual_out);
 582 | ab_status_t ab_job_get_process_maps(ab_object_t *job_obj, ab_koid_t target_proc_koid,
 583 |                                     ab_info_maps_t *out_maps, uint32_t max_maps,
 584 |                                     uint32_t *actual_out);
 585 | 
 586 | #endif /* ABOS_OBJECT_H */
 587 | 
 588 | 
 589 | 
 590 | 
```
### 12.13.3 Technical Notes

1. **Polymorphic C Single Inheritance (`ab_object_t base` at Offset 0)**: Every one of the 12 dispatcher structs (`ab_vmo_dispatcher_t`, `ab_vmar_dispatcher_t`, `ab_process_t`, `ab_log_dispatcher_t`, `ab_event_dispatcher_t`, `ab_eventpair_dispatcher_t`, `ab_port_dispatcher_t`, `ab_channel_dispatcher_t`, `ab_job_dispatcher_t`, `ab_thread_dispatcher_t`, `ab_resource_dispatcher_t`, `ab_interrupt_dispatcher_t`) places `ab_object_t base;` as its very first member. Under the ISO C standard (C11 6.7.2.1p15), a pointer to a structure object, suitably converted, points to its initial member and vice versa—allowing `handle_lookup()`, `state_tracker_*()`, and `ab_object_release()` to operate polymorphically on `ab_object_t *` with zero vtable indirection overhead!
2. **Why Peered Objects Require *Both* `handle_count` and `ref_count`**: Consider an entangled `ChannelDispatcher` pair `ch0 <-> ch1`. Each endpoint holds a raw kernel pointer (`peer`) to the other (`ref_count >= 1`). If ABOS only tracked `ref_count`, closing all user handles to `ch0` would drop `ref_count` from `2` to `1`—never reaching `0` and never notifying `ch1`! Separating `handle_count` (user-visible handle references) from `ref_count` (kernel memory lifetime) allows `handle_close()` to fire `on_zero_handles(ch0)` the instant `ch0->handle_count` hits `0`, asserting `AB_SIGNAL_PEER_CLOSED` on `ch1` and clearing `peer` pointers so `destroy()` reclaims heap memory cleanly once all references drain.
3. **Lock Hierarchy Inside `ab_process_t`**: `ab_process_t` intentionally splits `handle_lock` (protecting `handles[64]`) from `vmar_lock` (protecting `mappings[16]`). In `sys_vmar_map`, `handle_lookup()` acquires and releases `handle_lock` first, and `ab_vmar_map()` acquires `vmar_lock` afterward; similarly, during `copy_to_user()` inside handle-creating syscalls, `handle_lock` is already released before `resolve_user_page()` potentially acquires `vmar_lock` on a demand page fault.

---

## 12.14 include/syscalls.h

### 12.14.1 Functional Description

`include/syscalls.h` (53 lines) binds the `EL0` user-space C library (`ulibc`) and the `EL1` trap handler (`exceptions_c.c` + `syscalls.c`) to the exact same integer table of 34 system call IDs (`SYS_HANDLE_CLOSE = 1` through `SYS_VMAR_UNMAP = 34`), and declares the kernel-side dispatch, exit, and user-memory copy functions.

### 12.14.2 Heavily Commented Source Code

#### Program 12-13: `lesson_20/include/syscalls.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/syscalls.h" start="1" end="53" total="53" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/syscalls.h (53 lines)
       //// ==========================================================================
       //// Defines the 34 system call numbers shared with EL0 ulibc and declares
       //// the EL1 trap dispatcher, process exit routine, and safe user-copy APIs.
   1 | /* =========================================================================
   2 |  * syscalls.h - Zircon-Style Capability, Signal & Port System Calls
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_SYSCALLS_H
   5 | #define ABOS_SYSCALLS_H
   6 | 
   7 | #include <stdint.h>
   8 | #include "object.h"
   9 | #include "exceptions.h"
  10 | 
       //// Canonical ABOS System Call Table (1..34):
       //// Notice syscall number 0 is deliberately left unassigned so an
       //// uninitialized x8 == 0 register immediately fails with
       //// AB_ERR_NOT_SUPPORTED in syscall_dispatch()!
       ////   -  1..6  : Handle ops, DebugLog, Process Exit, Nanosleep
       ////   -  7..11 : Events, EventPairs, Signals, Synchronous Wait
       ////   - 12..15 : Ports & Asynchronous Object Wait
       ////   - 16..18 : Bidirectional Channels & Capability Transfer
       ////   - 19..22, 34 : VMOs & VMAR Map/Unmap
       ////   - 23..25 : Process & Thread Creation / Startup
       ////   - 26..30 : User-Space Driver DDK (Physical/Contiguous VMO, IRQs)
       ////   - 31..33 : Object Naming & 'ps'/'vmaps' Introspection
  11 | #define SYS_HANDLE_CLOSE        1
  12 | #define SYS_HANDLE_DUPLICATE    2
  13 | #define SYS_OBJECT_GET_INFO     3
  14 | #define SYS_DEBUGLOG_WRITE      4
  15 | #define SYS_PROCESS_EXIT        5
  16 | #define SYS_NANOSLEEP           6
  17 | #define SYS_EVENT_CREATE        7
  18 | #define SYS_EVENTPAIR_CREATE    8
  19 | #define SYS_OBJECT_SIGNAL       9
  20 | #define SYS_OBJECT_SIGNAL_PEER  10
  21 | #define SYS_OBJECT_WAIT_ONE     11
  22 | #define SYS_PORT_CREATE         12
  23 | #define SYS_PORT_QUEUE          13
  24 | #define SYS_PORT_WAIT           14
  25 | #define SYS_OBJECT_WAIT_ASYNC   15
  26 | #define SYS_CHANNEL_CREATE      16
  27 | #define SYS_CHANNEL_WRITE       17
  28 | #define SYS_CHANNEL_READ        18
  29 | #define SYS_VMO_CREATE          19
  30 | #define SYS_VMO_READ            20
  31 | #define SYS_VMO_WRITE           21
  32 | #define SYS_VMAR_MAP            22
  33 | #define SYS_PROCESS_CREATE      23
  34 | #define SYS_THREAD_CREATE       24
  35 | #define SYS_PROCESS_START       25
  36 | #define SYS_VMO_CREATE_PHYSICAL 26
  37 | #define SYS_VMO_CREATE_CONTIG   27
  38 | #define SYS_INTERRUPT_CREATE    28
  39 | #define SYS_INTERRUPT_WAIT      29
  40 | #define SYS_INTERRUPT_ACK       30
  41 | #define SYS_OBJECT_SET_NAME     31
  42 | #define SYS_JOB_GET_PS_INFO     32
  43 | #define SYS_PROCESS_GET_MAPS    33
  44 | #define SYS_VMAR_UNMAP          34
  45 | 
       //// EL1-only kernel entry points implemented in kernel/core/syscalls.c:
       //// syscall_dispatch() (called by exceptions_c.c), ab_process_exit_current()
       //// (called on SYS_PROCESS_EXIT and unhandled EL0 faults), and the validated
       //// physmap copy routines copy_from_user() / copy_to_user().
  46 | void syscall_dispatch(iframe_t *frame);
  47 | void ab_process_exit_current(int exit_code);
  48 | 
  49 | /* Safe EL0 <-> EL1 user memory copy helpers */
  50 | ab_status_t copy_from_user(void *kdst, const void *usrc, size_t len);
  51 | ab_status_t copy_to_user(void *udst, const void *ksrc, size_t len);
  52 | 
  53 | #endif /* ABOS_SYSCALLS_H */
```
### 12.14.3 Technical Notes

1. **Curriculum Evolution Reflected in Syscall Numbers**: Notice how `SYS_HANDLE_CLOSE` (1) through `SYS_PROCESS_GET_MAPS` (33) and `SYS_VMAR_UNMAP` (34) trace the incremental pedagogical progression of ABOS from Lesson 07 (basic handles and debuglog) -> Lesson 08 (events and signals) -> Lesson 09 (ports) -> Lesson 10 (channels) -> Lesson 11 (VMO/VMAR) -> Lesson 12 (jobs/processes/threads) -> Lesson 13 (DDK resources, DMA, interrupts) -> Lesson 17 (`ps`/`vmaps` introspection), preserving backward ABI compatibility at every step.

---

## 12.15 include/ipc_proto.h

### 12.15.1 Functional Description

`include/ipc_proto.h` (58 lines) defines the user-space service layer contracts shared across `main.c`, `userboot`, `ulibc`, `console-srv`, `fs-srv`, `shell`, and standalone utilities: the `USER_KVA_TO_UVA` bootstrap address macro, Zircon-inspired `AB_PA_*` startup handle type tags (`ab_proc_args_t`), the multi-VT Console IPC message format (`console_msg_t`), and the Filesystem VMO-serving IPC message format (`fs_msg_t`).

### 12.15.2 Heavily Commented Source Code

#### Program 12-14: `lesson_20/include/ipc_proto.h`

<!-- ECLK_CODE_LISTING file="lesson_20/include/ipc_proto.h" start="1" end="58" total="58" -->
```c
       //// ==========================================================================
       //// FILE OVERVIEW: include/ipc_proto.h (58 lines)
       //// ==========================================================================
       //// Defines the user-space bootstrap convention (AB_PA_* processargs) and
       //// the wire-level Channel IPC message protocols uniting userboot,
       //// console-srv, fs-srv, shell, and standalone user applications.
   1 | #ifndef ABOS_IPC_PROTO_H
   2 | #define ABOS_IPC_PROTO_H
   3 | 
   4 | #include <stdint.h>
   5 | #include "object.h"
   6 | #include "syscalls.h"
   7 | 
       //// USER_KVA_TO_UVA() converts the higher-half kernel link address of
       //// embedded userboot_entry (above 0xFFFFFFFF80000000) into its lower-half
       //// TTBR0_EL1 user virtual address when lk_main launches the root userboot
       //// process.
   8 | #define USER_KVA_TO_UVA(kva) ((vaddr_t)((uintptr_t)(kva) - 0xFFFFFFFF80000000ULL))
   9 | 
       //// Zircon-style ProcessArgs (AB_PA_*) bootstrap handle tags:
       //// When userboot or shell spawns a new standalone ELF/binary process, it
       //// sends an initial bootstrap message over the child's startup Channel
       //// containing an ab_proc_args_t header (magic = 0x41425041, ASCII 'ABPA')
       //// whose handle_types[0..7] array tells __ulibc_init() in crt_env.c the
       //// semantic role of each attached capability handle (Log, root VMAR,
       //// Console channel, FS channel, default Job, hardware Resource, or Server
       //// listener endpoint).
  10 | /* =========================================================================
  11 |  * Bootstrap Handle Type Tags (modeled after Zircon's PA_* processargs)
  12 |  * ========================================================================= */
  13 | #define AB_PA_NONE        0U
  14 | #define AB_PA_LOG         1U
  15 | #define AB_PA_VMAR_ROOT   2U
  16 | #define AB_PA_CONSOLE     3U
  17 | #define AB_PA_FS          4U
  18 | #define AB_PA_JOB_DEFAULT 5U
  19 | #define AB_PA_RESOURCE    6U
  20 | #define AB_PA_CHANNEL_SRV 7U
  21 | 
  22 | #define AB_PROC_ARGS_MAGIC 0x41425041U /* "ABPA" */
  23 | 
  24 | typedef struct {
  25 |     uint32_t magic;
  26 |     uint8_t  handle_types[8];
  27 | } ab_proc_args_t;
  28 | 
  29 | /* =========================================================================
       //// Console Server IPC Wire Protocol (NUM_VTS = 4 virtual terminals):
       ////   - Opcodes : WRITE (1), READ_LINE (2), SHUTDOWN (3), SWITCH_VT (4).
       ////   - console_msg_t : 4-byte 'op' + 240-byte 'text' = 244 bytes total,
       ////     sized deliberately <= CHANNEL_MAX_MSG_BYTES (256 bytes) so every
       ////     console transaction fits in a single atomic Channel datagram!
  30 |  * IPC Protocol Definitions between Shell <-> Console Server & FS Server
  31 |  * ========================================================================= */
  32 | #define NUM_VTS           4U
  33 | 
  34 | #define CON_OP_WRITE      1U
  35 | #define CON_OP_READ_LINE  2U
  36 | #define CON_OP_SHUTDOWN   3U
  37 | #define CON_OP_SWITCH_VT  4U
  38 | 
  39 | typedef struct {
  40 |     uint32_t op;
  41 |     char     text[240];
  42 | } console_msg_t;
  43 | 
       //// Filesystem Server (BOOTFS / virtio-blk) IPC Wire Protocol:
       ////   - Opcodes  : FS_OP_LIST (1), FS_OP_OPEN_VMO (2, returns a read-only
       ////                executable VMO handle attached to the reply message!),
       ////                and FS_OP_SHUTDOWN (3).
       ////   - fs_msg_t : 3 * 4-byte header fields + 236-byte payload = 248 bytes
       ////                total (again <= 256-byte CHANNEL_MAX_MSG_BYTES).
  44 | #define FS_OP_LIST        1U
  45 | #define FS_OP_OPEN_VMO    2U
  46 | #define FS_OP_SHUTDOWN    3U
  47 | 
  48 | typedef struct {
  49 |     uint32_t op;
  50 |     uint32_t status;
  51 |     uint32_t file_size;
  52 |     char     data[236];
  53 | } fs_msg_t;
  54 | 
       //// Entry symbol of the bootstrap loader (user/userboot/userboot.c) mapped
       //// into the first EL0 process by lk_main().
  55 | /* Embedded userboot bootstrap entry point */
  56 | void userboot_entry(uint64_t arg0);
  57 | 
  58 | #endif /* ABOS_IPC_PROTO_H */
```
### 12.15.3 Technical Notes

1. **Capability-Based Service Discovery via `ab_proc_args_t`**: In UNIX, a newly `exec()`'d program discovers the console via ambient `fd 0/1/2` and opens files via global `/` pathnames. In ABOS, a newly started process receives *only* a single bootstrap `Channel` handle in `x0`. Its C runtime (`__ulibc_init` in `user/ulibc/crt_env.c`) calls `sys_channel_read()` once on that channel to receive an `ab_proc_args_t` message (`magic == 0x41425041`, `"ABPA"`) plus up to 8 handles tagged with `AB_PA_LOG`, `AB_PA_VMAR_ROOT`, `AB_PA_CONSOLE`, `AB_PA_FS`, `AB_PA_JOB_DEFAULT`, etc. If a parent omits `AB_PA_FS` or `AB_PA_JOB_DEFAULT` from that startup packet, the child process is mathematically sandboxed—it cannot even name the filesystem server or task tree!
2. **Zero-Copy Executable Loading via `FS_OP_OPEN_VMO`**: When `shell` spawns an external program (such as `ps.bin` or `vmaps.bin`), it sends an `fs_msg_t` with `op = FS_OP_OPEN_VMO` and the filename in `data[]` to `fs-srv`. Rather than streaming kilobytes of binary data over 256-byte channel messages, `fs-srv` replies with a 248-byte `fs_msg_t` *plus a read-only/executable `VMO` capability handle* attached in `handles[0]`—showcasing how Channels and VMOs compose in a Zircon microkernel.

---

## 12.16 Summary

Together, the 14 headers in `lesson_20/include/` (`1,213` lines) capture the entire static architecture of ABOS:
- **Hardware & LK Foundation (`spinlock.h`, `debuglog.h`, `dtb.h`, `exceptions.h`, `gic.h`, `timer.h`, `pmm.h`, `heap.h`, `mmu.h`, `thread.h`, `wait.h`)**: Cleanly encapsulates AArch64 registers, `physmap`/`periphmap` address translation, 16-byte page frame metadata, ASID-tagged user address spaces, 8 KB kernel stacks, and wait-queue blocking without leaking Zircon policy downward.
- **Zircon Object, Syscall & Service Contracts (`object.h`, `syscalls.h`, `ipc_proto.h`)**: Builds all 12 capability dispatchers on a single polymorphic `ab_object_t` header with dual reference/handle counts, exposes 34 capability-checked `SVC #0` system calls, and standardizes the `"ABPA"` bootstrap protocol and 256-byte-bounded `Console`/`FS` IPC messages that bring the multi-server `EL0` userland to life.
