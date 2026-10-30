# 8 Exceptions, Interrupts, Timer and SMP Scheduler

A microkernel spends most of its active lifetime responding to transitions across the hardware exception boundary: an unprivileged EL0 server invokes `svc #0` to transfer a capability over a Channel, a newly spawned application faults on an uncommitted Virtual Memory Object (VMO) page, a user keystroke asserts PL011 UART IRQ 33, or the 100 Hz ARMv8 Generic Timer fires on all four Cortex-A53 cores to enforce preemptive round-robin time-slicing.

In ABOS, these mechanisms are implemented across **eight tightly cohesive files** totaling 875 lines of assembly and C:
1. **`kernel/core/exceptions.S`** (*Program 8-1*, 130 lines): The 2 KB-aligned `VBAR_EL1` exception vector table and symmetrical `regsave` / `regrestore` macros constructing 288-byte `iframe_t` trap frames.
2. **`kernel/core/exceptions_c.c`** (*Program 8-2*, 86 lines): Synchronous trap decoding (`SVC` syscalls, VMO demand paging on `EC 0x20`/`0x24`, EL0 crash isolation vs EL1 panic) and GICv2 IRQ dispatch + preemption.
3. **`kernel/dev/gic.c`** (*Program 8-3*, 48 lines): ARM GICv2 Distributor (`GICD_*`) and banked per-CPU Interface (`GICC_*`) driver over the Higher-Half `Periphmap`.
4. **`kernel/dev/timer.c`** (*Program 8-4*, 51 lines): Per-CPU 100 Hz ARMv8 Physical Timer (`PPI 30`) and overflow-safe 64-bit monotonic clock `current_time_ns()`.
5. **`kernel/core/switch.S`** (*Program 8-5*, 42 lines): Synchronous AAPCS64 kernel-stack switcher `arm64_context_switch()` saving 112-byte `context_switch_frame_t` records.
6. **`kernel/core/uspace_entry.S`** (*Program 8-6*, 50 lines): Register-scrubbing privilege drop (`eret` into `EL0t`) launching new user threads.
7. **`kernel/core/thread.c`** (*Program 8-7*, 317 lines): 4-core SMP preemptive round-robin scheduler, thread creation/exit, lazy `TTBR0_EL1` switching, and SMP-safe deferred thread reaping.
8. **`kernel/core/wait.c`** (*Program 8-8*, 151 lines): Allocation-free intrusive FIFO `wait_queue_t` and dual-mode `event_t` primitives.

---

## 8.1 Main Functions

### The Two-Frame Kernel Stack Architecture (`iframe_t` vs `context_switch_frame_t`)

The single most important mental model for understanding Chapters 8 and 10 is how **two distinct register frames** nest on a user thread's 8 KB EL1 kernel stack (`thread_t.stack_base .. stack_base + 8192`) whenever an EL0 thread traps into the kernel and blocks (e.g., inside `sys_port_wait`) or is preempted by a timer IRQ:

```text
  8 KB Per-Thread EL1 Kernel Stack (Full-Descending, 16-Byte Aligned)
  ===================================================================
  stack_base + 8192 (kstack_top) -> +---------------------------------------+
                                    | iframe_t (288 bytes, exceptions.S)    |
                                    |  * Saved at EL0 -> EL1 trap/IRQ entry |
                                    |  * x0..x29, x30(lr), sp_el0(usp),     |
                                    |    elr_el1, spsr_el1, mdscr_el1, pad  |
                   sp at C entry -> +---------------------------------------+
                                    | C Activation Frames (exceptions_c.c,  |
                                    |  syscall_dispatch / arm64_irq_exc,    |
                                    |  thread_yield / wait_queue_block,     |
                                    |  sched_reschedule_locked)             |
                                    +---------------------------------------+
               thread->kernel_sp -> | context_switch_frame_t (112 bytes,    |
                                    |  switch.S: arm64_context_switch)      |
                                    |  * tpidr_el0, tpidrro_el0             |
                                    |  * x19..x29, x30 (return into sched)  |
                                    +---------------------------------------+
                                    | Unused stack space (~7.4 KB free)     |
                      stack_base -> +---------------------------------------+
```

Why split CPU state across `iframe_t` (288 bytes) and `context_switch_frame_t` (112 bytes) instead of copying registers into `thread_t`?
1. **Zero Redundant Register Saves on Context Switch**: When an EL0 thread is interrupted asynchronously, `regsave` in `exceptions.S` already saved all 31 user general-purpose registers plus `SP_EL0`, `ELR_EL1`, and `SPSR_EL1` into `iframe_t` at the top of its kernel stack. Later, if the kernel decides to block or preempt that thread inside C code, `arm64_context_switch()` in `switch.S` only needs to push the **12 AAPCS64 callee-saved registers (`x19..x30`) plus the 2 TLS registers (`tpidr_el0`, `tpidrro_el0`)** — 112 bytes total!
2. **Preemptible Blocking Syscalls**: Because `arm64_sync_exception()` re-enables interrupts (`arch_enable_ints()`) around `syscall_dispatch(iframe)`, a thread blocked inside `sys_channel_read()` or preempted mid-syscall can even take a nested EL1 timer IRQ (which pushes a *second* `iframe_t` deeper on the same 8 KB stack!), switch to another thread via `context_switch_frame_t`, migrate to a different CPU core when woken, unwind its C frames back to `.Lel0_sync`, and `eret` cleanly to EL0!

### End-to-End Interrupt, Preemption, and Synchronization Pipeline

| Subsystem File | Hardware / Software Interface | Key SMP & Architectural Invariants |
| :--- | :--- | :--- |
| `exceptions.S` | `VBAR_EL1` (2 KB aligned, 16 × 128 B slots) | Saves `sp_el0`, `elr_el1`, `spsr_el1` per-thread so nested IRQs and migrations never clobber return state |
| `exceptions_c.c` | `ESR_EL1`, `FAR_EL1` | Re-enables IRQs during EL0 `SVC`; demand-pages `FSC 0x04..0x07`; calls `GICC_EOIR` *before* `thread_preempt()` |
| `gic.c` | `GICD_*` (`+0x08000000`), `GICC_*` (`+0x08010000`) | Uses atomic write-1-to-set/clear (`ISENABLER`/`ICENABLER`); routes SPIs (`>= 32`) to CPU 0 |
| `timer.c` | `CNTP_TVAL_EL0`, `CNTPCT_EL0` (`PPI 30`) | Reloads `CNTP_TVAL_EL0` inside handler to de-assert level-sensitive PPI 30; splits `secs`/`rem` to avoid overflow |
| `switch.S` | `SP_EL1`, `tpidr_el0`, `x19..x30` | Pivots stacks in 1 instruction (`mov sp, x1`); launches new threads via `frame->lr = initial_thread_trampoline` |
| `uspace_entry.S` | `SP_EL1`, `SP_EL0`, `ELR_EL1`, `SPSR_EL1` | Resets `SP_EL1` to `kstack_top`, sets `SPSR_EL1 = 0` (`EL0t`, IRQs live), zeroes `x2..x30` before `eret` |
| `thread.c` | `sched_lock`, `thread_table[64]`, `idle_threads[4]` | Cross-thread `sched_lock` handoff; verifies `!on_cpu` across all 4 cores before reaping dead thread stacks |
| `wait.c` | `wait_queue_t`, `event_t` | Shares `sched_lock` with scheduler to prevent lost wakeups; supports sticky broadcast and `AUTOUNSIGNAL` modes |

---

## 8.2 `kernel/core/exceptions.S`

### 8.2.1 Functional Description

When an exception or interrupt occurs in AArch64, the processor hardware performs four actions atomically before fetching the first instruction of the vector handler:
1. Saves the return address into **`ELR_EL1`** (for `SVC`, `PC + 4`; for synchronous faults and asynchronous IRQs, the exact unexecuted `PC`).
2. Copies the current `PSTATE` (`NZCV`, `DAIF`, execution state, and source Exception Level) into **`SPSR_EL1`**.
3. Records the trap cause in **`ESR_EL1`** (and faulting virtual address in **`FAR_EL1`** for memory aborts — note asynchronous IRQs do *not* modify `ESR_EL1` or `FAR_EL1`).
4. Switches to **EL1h** (selecting `SP_EL1`), sets `PSTATE.{D,A,I,F} = 0b1111` (masking further interrupts!), and branches to `VBAR_EL1 + group_offset + type_offset`.

ARMv8 divides the 2048-byte (`0x800`) vector table into **4 origin groups × 4 exception types**, giving each of the 16 entries exactly `128 bytes` (`32` four-byte instructions):

| Vector Offset | Origin Group | Sync (`+0x000`) | IRQ (`+0x080`) | FIQ (`+0x100`) | SError (`+0x180`) |
| :---: | :--- | :--- | :--- | :--- | :--- |
| `0x000..0x1FF` | Current EL with `SP_EL0` (`EL1t`) | `.Lel1_sync` | `.Lel1_irq` | `.Lel1_unhandled` | `.Lel1_unhandled` |
| `0x200..0x3FF` | Current EL with `SP_ELx` (`EL1h`) | `.Lel1_sync` | `.Lel1_irq` | `.Lel1_unhandled` | `.Lel1_unhandled` |
| `0x400..0x5FF` | Lower EL (`EL0`), AArch64 | **`.Lel0_sync`** | **`.Lel0_irq`** | `.Lel1_unhandled` | `.Lel1_unhandled` |
| `0x600..0x7FF` | Lower EL (`EL0`), AArch32 | `.Lel1_unhandled` | `.Lel1_unhandled` | `.Lel1_unhandled` | `.Lel1_unhandled` |

Because `regsave` + C call + `regrestore` exceeds 32 instructions, each 128-byte slot in `arm64_el1_exception_base` contains a single `b .L<handler>` jump to the out-of-line bodies immediately following the table.

### 8.2.2 Heavily Commented Source Code
#### Program 8-1: `lesson_20/kernel/core/exceptions.S`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/exceptions.S" start="1" end="130" total="130" -->
```asm
       ## kernel/core/exceptions.S defines the AArch64 EL1 Exception Vector Table
       ## (installed into VBAR_EL1 at line 146 of start.S) together with the symmetrical
       ## regsave and regrestore assembly macros. Every synchronous trap (EL0 'svc #0'
       ## system call, VMO demand-paging translation fault, or crash) and every
       ## asynchronous hardware IRQ (100 Hz Generic Timer PPI 30 or PL011 UART SPI 33)
       ## enters and exits the microkernel through this 130-line file.
   1 | /* =========================================================================
   2 |  * exceptions.S - EL1 Exception Vector Table & iframe_t Save/Restore Macros
   3 |  * References:
   4 |  *   - zircon_org/kernel/arch/arm64/exceptions.S
   5 |  * ========================================================================= */
   6 | 
       ## Constants shared with include/exceptions.h:
       ## - ARM64_EXCEPTION_FLAG_LOWER_EL (0x1): Passed as exception_flags (w1) to C handlers when the trap originated in unprivileged EL0 user space rather than EL1 kernel space.
       ## - IFRAME_SIZE (288 bytes = 18 * 16 bytes): Exact byte size of struct arm64_iframe (iframe_t). Because 288 is an exact multiple of 16, subtracting/adding IFRAME_SIZE preserves AArch64's mandatory 16-byte hardware SP_EL1 alignment (SCTLR_EL1.SA).
   7 | #define ARM64_EXCEPTION_FLAG_LOWER_EL 0x1
   8 | #define IFRAME_SIZE 288
   9 | 
       ## regsave macro: Allocates 288 bytes on the current thread's EL1 kernel stack
       ## (SP_EL1, selected automatically by hardware upon exception entry into EL1h)
       ## and snapshots general-purpose registers x0..x29 in fifteen 16-byte Store-Pair
       ## (stp) instructions at offsets 0..239. Even if the interrupted context was
       ## running in EL0, hardware has already banked SP to SP_EL1 before executing the
       ## first instruction of the vector slot, so untrusted user SP_EL0 is never
       ## dereferenced here!
  10 | .macro regsave, flags
  11 |     sub     sp, sp, #IFRAME_SIZE
  12 |     stp     x0,  x1,  [sp, #(16 * 0)]
  13 |     stp     x2,  x3,  [sp, #(16 * 1)]
  14 |     stp     x4,  x5,  [sp, #(16 * 2)]
  15 |     stp     x6,  x7,  [sp, #(16 * 3)]
  16 |     stp     x8,  x9,  [sp, #(16 * 4)]
  17 |     stp     x10, x11, [sp, #(16 * 5)]
  18 |     stp     x12, x13, [sp, #(16 * 6)]
  19 |     stp     x14, x15, [sp, #(16 * 7)]
  20 |     stp     x16, x17, [sp, #(16 * 8)]
  21 |     stp     x18, x19, [sp, #(16 * 9)]
  22 |     stp     x20, x21, [sp, #(16 * 10)]
  23 |     stp     x22, x23, [sp, #(16 * 11)]
  24 |     stp     x24, x25, [sp, #(16 * 12)]
  25 |     stp     x26, x27, [sp, #(16 * 13)]
  26 |     stp     x28, x29, [sp, #(16 * 14)]
  27 | 
       ## Save x30 (LR) plus the four critical banked exception/user state registers
       ## into the top 48 bytes (offsets 240..287) of iframe_t:
       ## - sp_el0    (offset 248): User-mode stack pointer! Saving sp_el0 inside the per-thread iframe_t is mandatory in a preemptive kernel: if Thread A traps from EL0 and blocks or is preempted by Thread B, Thread B will overwrite the hardware sp_el0 register with its own user stack pointer.
       ## - elr_el1   (offset 256): Exception Link Register (faulting PC for aborts/IRQs, or PC + 4 after 'svc #0' for syscalls). Must be saved per-thread because blocking syscalls re-enable IRQs at line 30 of exceptions_c.c, allowing nested timer IRQs to overwrite hardware ELR_EL1!
       ## - spsr_el1  (offset 264): Saved Program Status Register (interrupted PSTATE: NZCV condition flags, DAIF interrupt masks, and M[3:0] target exception level EL0t vs EL1h).
       ## - mdscr_el1 (offset 272): Monitor Debug System Control Register (saved alongside xzr 8-byte alignment pad at offset 280, matching Zircon's arm64_iframe_long).
  28 |     mrs     x9,  sp_el0
  29 |     mrs     x10, elr_el1
  30 |     mrs     x11, spsr_el1
  31 |     mrs     x12, mdscr_el1
  32 |     stp     x30, x9,  [sp, #(16 * 15)]
  33 |     stp     x10, x11, [sp, #(16 * 16)]
  34 |     stp     x12, xzr, [sp, #(16 * 17)]
  35 | .endm
  36 | 
       ## regrestore macro: Exact mirror image of regsave executed immediately before
       ## returning from the exception.
       ## 1. Reloads x30 and restores sp_el0, elr_el1, spsr_el1, and mdscr_el1 using
       ## scratch registers x9..x12 BEFORE restoring x9..x12 themselves on lines 50-51!
       ## 2. Reloads x0..x29 from offsets 0..239 (note: for an EL0 system call,
       ## syscall_dispatch() wrote the syscall return status into iframe->r[0], so line
       ## 46 loads that return value directly into hardware x0!).
       ## 3. Deallocates the 288-byte iframe ('add sp, sp, #IFRAME_SIZE') and executes
       ## 'eret', atomically copying ELR_EL1 -> PC and SPSR_EL1 -> PSTATE.
  37 | .macro regrestore
  38 |     ldp     x30, x9,  [sp, #(16 * 15)]
  39 |     ldp     x10, x11, [sp, #(16 * 16)]
  40 |     ldp     x12, xzr, [sp, #(16 * 17)]
  41 |     msr     sp_el0,    x9
  42 |     msr     elr_el1,   x10
  43 |     msr     spsr_el1,  x11
  44 |     msr     mdscr_el1, x12
  45 | 
  46 |     ldp     x0,  x1,  [sp, #(16 * 0)]
  47 |     ldp     x2,  x3,  [sp, #(16 * 1)]
  48 |     ldp     x4,  x5,  [sp, #(16 * 2)]
  49 |     ldp     x6,  x7,  [sp, #(16 * 3)]
  50 |     ldp     x8,  x9,  [sp, #(16 * 4)]
  51 |     ldp     x10, x11, [sp, #(16 * 5)]
  52 |     ldp     x12, x13, [sp, #(16 * 6)]
  53 |     ldp     x14, x15, [sp, #(16 * 7)]
  54 |     ldp     x16, x17, [sp, #(16 * 8)]
  55 |     ldp     x18, x19, [sp, #(16 * 9)]
  56 |     ldp     x20, x21, [sp, #(16 * 10)]
  57 |     ldp     x22, x23, [sp, #(16 * 11)]
  58 |     ldp     x24, x25, [sp, #(16 * 12)]
  59 |     ldp     x26, x27, [sp, #(16 * 13)]
  60 |     ldp     x28, x29, [sp, #(16 * 14)]
  61 |     add     sp, sp, #IFRAME_SIZE
  62 |     eret
  63 | .endm
  64 | 
       ## Place arm64_el1_exception_base in .text aligned to 2^11 = 2048 bytes (0x800).
       ## ARMv8-A hardware clears bits [10:0] of VBAR_EL1 as RES0 and computes vector
       ## entry addresses by ORing a 4-bit slot index shifted by 7 (slot * 0x80) into
       ## VBAR_EL1[63:11]. Misaligning this symbol to anything smaller than 2 KB would
       ## silently corrupt vector dispatch!
  65 | .section .text
  66 | .align 11
  67 | .global arm64_el1_exception_base
  68 | arm64_el1_exception_base:
       ## Group 0 (Offsets 0x000..0x180) — Exceptions taken from Current EL (EL1) while
       ## using SP_EL0 (EL1t). ABOS always runs EL1 in EL1h mode (using dedicated
       ## SP_EL1), so these four 128-byte ('.align 7' = 2^7 = 128) slots are defensive
       ## fallbacks routing Sync/IRQ to .Lel1_sync/.Lel1_irq and FIQ/SError to
       ## .Lel1_unhandled.
  69 |     /* Current EL with SP_EL0 */
  70 |     .align 7; b .Lel1_sync
  71 |     .align 7; b .Lel1_irq
  72 |     .align 7; b .Lel1_unhandled
  73 |     .align 7; b .Lel1_unhandled
  74 | 
       ## Group 1 (Offsets 0x200..0x380) — Exceptions taken from Current EL (EL1) while
       ## using SP_ELx (EL1h):
       ## - Slot +0x200 (.Lel1_sync): Kernel-mode synchronous traps (e.g., an unhandled kernel NULL pointer dereference -> [PANIC] diagnostic dump).
       ## - Slot +0x280 (.Lel1_irq): Kernel-mode hardware IRQs! Fired whenever a 100 Hz timer tick or UART IRQ arrives while a CPU is idle in thread_become_idle()'s 'wfi' instruction or executing inside syscall_dispatch() with interrupts enabled.
  75 |     /* Current EL with SP_ELx (Kernel mode exceptions & IRQs) */
  76 |     .align 7; b .Lel1_sync
  77 |     .align 7; b .Lel1_irq
  78 |     .align 7; b .Lel1_unhandled
  79 |     .align 7; b .Lel1_unhandled
  80 | 
       ## Group 2 (Offsets 0x400..0x580) — Exceptions taken from a Lower EL (EL0 user
       ## space) running 64-bit AArch64:
       ## - Slot +0x400 (.Lel0_sync): Every 'svc #0' system call, every VMO lazy demand-paging translation fault (Instruction/Data Abort), and any user-space segmentation/alignment crash enters here!
       ## - Slot +0x480 (.Lel0_irq): Preemptive timer ticks and peripheral IRQs arriving while a CPU is executing EL0 application or system-server code.
  81 |     /* Lower EL using AArch64 (User mode EL0 syscalls, faults & IRQs) */
  82 |     .align 7; b .Lel0_sync
  83 |     .align 7; b .Lel0_irq
  84 |     .align 7; b .Lel1_unhandled
  85 |     .align 7; b .Lel1_unhandled
  86 | 
       ## Group 3 (Offsets 0x600..0x780) — Exceptions taken from a Lower EL running
       ## 32-bit AArch32. ABOS is a pure 64-bit AArch64 microkernel, so all four AArch32
       ## slots route to .Lel1_unhandled.
  87 |     /* Lower EL using AArch32 */
  88 |     .align 7; b .Lel1_unhandled
  89 |     .align 7; b .Lel1_unhandled
  90 |     .align 7; b .Lel1_unhandled
  91 |     .align 7; b .Lel1_unhandled
  92 | 
       ## .Lel1_sync: Out-of-line body for EL1 synchronous exceptions (placed after the
       ## 2 KB vector table because each inline vector slot holds at most 32
       ## instructions = 128 bytes, whereas regsave + call + regrestore is 40+
       ## instructions!). Passes x0 = &iframe (sp), w1 = 0 (kernel origin), and x2 =
       ## esr_el1 to arm64_sync_exception().
  93 | .Lel1_sync:
  94 |     regsave 0
  95 |     mov     x0, sp
  96 |     mov     w1, #0
  97 |     mrs     x2, esr_el1
  98 |     bl      arm64_sync_exception
  99 |     regrestore
 100 | 
       ## .Lel1_irq: Out-of-line body for EL1 hardware IRQs. Saves iframe_t, passes x0 =
       ## sp and w1 = 0 to arm64_irq_exception(), and restores the interrupted EL1
       ## context (potentially after thread_preempt() switched to another thread and
       ## later switched back!).
 101 | .Lel1_irq:
 102 |     regsave 0
 103 |     mov     x0, sp
 104 |     mov     w1, #0
 105 |     bl      arm64_irq_exception
 106 |     regrestore
 107 | 
       ## .Lel0_sync: Out-of-line body for EL0 synchronous exceptions. Passes w1 =
       ## ARM64_EXCEPTION_FLAG_LOWER_EL (0x1) so arm64_sync_exception() knows the trap
       ## came from unprivileged user mode — authorizing 'svc #0' dispatch and isolating
       ## fatal faults to the offending user process rather than panicking the kernel.
 108 | .Lel0_sync:
 109 |     regsave ARM64_EXCEPTION_FLAG_LOWER_EL
 110 |     mov     x0, sp
 111 |     mov     w1, #ARM64_EXCEPTION_FLAG_LOWER_EL
 112 |     mrs     x2, esr_el1
 113 |     bl      arm64_sync_exception
 114 |     regrestore
 115 | 
       ## .Lel0_irq: Out-of-line body for EL0 hardware IRQs. Identical to .Lel1_irq
       ## except w1 = ARM64_EXCEPTION_FLAG_LOWER_EL. Note that ARMv8 hardware
       ## automatically masks PSTATE.I upon vectoring here, so interrupts remain
       ## disabled throughout regsave, GIC acknowledge/EOI, and regrestore unless
       ## explicitly unmasked.
 116 | .Lel0_irq:
 117 |     regsave ARM64_EXCEPTION_FLAG_LOWER_EL
 118 |     mov     x0, sp
 119 |     mov     w1, #ARM64_EXCEPTION_FLAG_LOWER_EL
 120 |     bl      arm64_irq_exception
 121 |     regrestore
 122 | 
       ## .Lel1_unhandled: Catch-all handler for FIQ, SError, and unexpected AArch32
       ## vectors. Snapshots registers into an iframe_t and invokes
       ## arm64_sync_exception(..., 0, esr_el1) to print a full crash register dump
       ## before halting the core in a low-power WFE loop.
 123 | .Lel1_unhandled:
 124 |     regsave 0
 125 |     mov     x0, sp
 126 |     mov     w1, #0
 127 |     mrs     x2, esr_el1
 128 |     bl      arm64_sync_exception
 129 | 1:  wfe
 130 |     b       1b
```
### 8.2.3 Technical Notes

1. **Why `regrestore` Reloads System Registers *Before* `x8..x13`**:
   Look at the carefully chosen instruction order in `regrestore` (lines 38–51): `ldp x30, x9`, `ldp x10, x11`, `ldp x12, xzr` use `x9..x12` as temporary staging registers to execute `msr sp_el0, x9`, `msr elr_el1, x10`, `msr spsr_el1, x11`, and `msr mdscr_el1, x12`. Only *after* those four `msr` instructions complete do lines 50–51 reload the interrupted thread's true `x8..x13` values from `[sp, #64]` and `[sp, #80]`. Notice also line 40: `ldp x12, xzr, [sp, #(16 * 17)]` loads the 8-byte alignment pad into the architectural zero register `xzr`, discarding it cleanly in a single paired load without wasting another scratch register!

2. **Why `SP_EL1` Corruption Is Impossible Even If EL0 Sets `SP = 0xDEADBEEF`**:
   Beginners coming from older architectures sometimes worry: *"What if a buggy or malicious EL0 program sets `sp` to an unmapped address or `0xFFFFFFFF80080000` and executes `svc #0`?"* Because `start.S` configured `SPSR` to run the kernel in **EL1h** (`M[0] == 1`, Handler mode using dedicated `SP_EL1`), EL0's `sp` instruction modifies **only `SP_EL0`**. When `svc #0` traps to EL1h, hardware switches the `sp` alias to the current kernel thread's trusted Higher-Half `SP_EL1` (`kstack_top`), while the untrusted user pointer sits inertly inside system register `sp_el0` (`iframe->usp`).

3. **Comparison with Zircon (`zircon/kernel/arch/arm64/exceptions.S`)**:
   Zircon distinguishes `arm64_iframe_long` (full 30 GPRs saved on synchronous exceptions and lower-EL entries) from `arm64_iframe_short` (saving only caller-saved `x0..x18` + `x29, x30` on kernel-mode IRQs where AAPCS64 guarantees the C IRQ handler preserves `x19..x28`). ABOS uses the uniform 288-byte `iframe_t` across all vectors so `dump_iframe()` and debugger inspection have a single canonical frame layout everywhere.

---

## 8.3 `kernel/core/exceptions_c.c`

### 8.3.1 Functional Description

`kernel/core/exceptions_c.c` implements the C policy layer atop `exceptions.S`:

```text
  arm64_sync_exception(iframe, flags, esr)
   |-- Read FAR_EL1; extract EC = esr[31:26], ISS = esr[24:0]
   |
   |-- [1] EC == 0x15 (SVC64) && LOWER_EL?
   |         +-- arch_enable_ints() -> syscall_dispatch(iframe) -> arch_disable_ints() -> return
   |
   |-- [2] EC == 0x20 (Inst Abort) or 0x24 (Data Abort) && FSC in 0x04..0x07 (Translation Fault)?
   |         +-- ab_process_handle_page_fault(proc, far, is_write, is_exec) == 0? -> return (retry!)
   |
   |-- [3] flags & LOWER_EL? (Unhandled User Fault: NULL deref, RO write, bad PC)
   |         +-- Log [EXCEPTION] + dump_iframe -> arch_enable_ints() -> ab_process_exit_current(-1)
   |
   +-- [4] Otherwise (Unhandled EL1 Kernel Fault)
             +-- Log [PANIC] + dump_iframe -> halt CPU in wfe loop
```

And for hardware interrupts:

```text
  arm64_irq_exception(iframe, flags)
   |-- iar = gic_acknowledge_interrupt() ; irq = iar & 0x3FF
   |-- if (irq >= 1020) return;  (Spurious GIC ID 1023)
   |-- if (irq == 30) timer_irq_handler()
   |   else           ab_interrupt_dispatch_hw_irq(irq)
   |-- gic_end_interrupt(iar)    (Drop GIC active priority FIRST!)
   +-- thread_preempt()          (Yield if need_resched[cpu] == 1)
```

### 8.3.2 Heavily Commented Source Code
#### Program 8-2: `lesson_20/kernel/core/exceptions_c.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/exceptions_c.c" start="1" end="86" total="86" -->
```c
       //// kernel/core/exceptions_c.c is the C-level trap and interrupt dispatcher
       //// invoked by exceptions.S. In just 86 lines, it implements four foundational
       //// microkernel mechanisms:
       //// 1. EL0 system call entry with preemptible/blocking IRQ enablement.
       //// 2. Transparent VMO lazy demand paging on EL0 Instruction and Data Translation
       //// Faults.
       //// 3. Fault isolation: terminating a crashing EL0 process cleanly while keeping
       //// the kernel and sibling servers alive.
       //// 4. GICv2 hardware interrupt dispatch (routing PPI 30 to the scheduler timer
       //// and peripheral SPIs to user-space InterruptDispatchers) followed by preemptive
       //// rescheduling.
   1 | /* =========================================================================
   2 |  * exceptions_c.c - Exception, Syscall & VMO Demand-Paging Handler
   3 |  * ========================================================================= */
   4 | #include "exceptions.h"
   5 | #include "debuglog.h"
   6 | #include "spinlock.h"
   7 | #include "gic.h"
   8 | #include "timer.h"
   9 | #include "thread.h"
  10 | #include "object.h"
  11 | #include "syscalls.h"
  12 | 
       //// dump_iframe(): Diagnostic crash printer emitting the faulting CPU number,
       //// argument/return registers x0..x3, faulting PC (elr), saved processor state
       //// (spsr), and user stack pointer (usp = sp_el0) at CRITICAL priority.
  13 | void dump_iframe(const iframe_t *iframe) {
  14 |     dprintf(CRITICAL, "iframe %p (CPU %u):\n", (void *)iframe, arch_curr_cpu_num());
  15 |     dprintf(CRITICAL, " x0  %p  x1  %p  x2  %p  x3  %p\n",
  16 |             (void *)iframe->r[0], (void *)iframe->r[1],
  17 |             (void *)iframe->r[2], (void *)iframe->r[3]);
  18 |     dprintf(CRITICAL, " elr %p  spsr 0x%lx  usp %p\n",
  19 |             (void *)iframe->elr, iframe->spsr, (void *)iframe->usp);
  20 | }
  21 | 
       //// arm64_sync_exception(): Decodes the ARMv8 Exception Syndrome Register
       //// (ESR_EL1) and reads the Fault Address Register (FAR_EL1):
       //// - ec (bits [31:26], 6-bit Exception Class): Identifies the exact cause of the synchronous trap (e.g., 0x15 = AArch64 SVC, 0x20 = Lower-EL Instruction Abort, 0x21 = Same-EL Instruction Abort, 0x24 = Lower-EL Data Abort, 0x25 = Same-EL Data Abort).
       //// - iss (bits [24:0], 25-bit Instruction Specific Syndrome): Carries fault status codes (FSC), read/write direction (WnR), or the immediate operand of 'svc #imm'.
       //// - far_el1: Holds the exact 64-bit faulting virtual address on Instruction/Data/PC-alignment aborts.
  22 | void arm64_sync_exception(iframe_t *iframe, uint32_t exception_flags, uint32_t esr) {
  23 |     uint32_t ec  = (esr >> 26) & 0x3F;
  24 |     uint32_t iss = esr & 0x1FFFFFF;
  25 |     uint64_t far;
  26 |     __asm__ volatile("mrs %0, far_el1" : "=r"(far));
  27 | 
       //// Fast Path 1 — System Call Dispatch (EC == 0x15 from EL0):
       //// When an EL0 thread executes 'svc #0', hardware advances ELR_EL1 to the
       //// instruction AFTER 'svc #0' and vectors here with PSTATE.I (IRQs) masked.
       //// Look closely at the three-line bracket on lines 30-32:
       ////   arch_enable_ints();
       ////   syscall_dispatch(iframe);
       ////   arch_disable_ints();
       //// Why re-enable interrupts across syscall_dispatch(), and why MUST we disable
       //// them again before returning?
       //// - Enabling IRQs allows blocking syscalls (sys_channel_read/wait, sys_port_wait, sys_interrupt_wait, sys_nanosleep) to receive timer wakeups and hardware UART/VirtIO interrupts even on a single core!
       //// - Disabling IRQs before returning to exceptions.S protects regrestore (lines 38-62 of Program 8-1): once regrestore loads ELR_EL1 and SPSR_EL1 into hardware registers prior to 'eret', taking a nested EL1 IRQ before 'eret' executes would clobber ELR_EL1/SPSR_EL1!
  28 |     /* EC 0x15 = SVC instruction from AArch64 (System Call) */
  29 |     if (ec == 0x15 && (exception_flags & ARM64_EXCEPTION_FLAG_LOWER_EL)) {
  30 |         arch_enable_ints();
  31 |         syscall_dispatch(iframe);
  32 |         arch_disable_ints();
  33 |         return;
  34 |     }
  35 | 
       //// Fast Path 2 — Lazy VMO Demand Paging (EC == 0x20 Instruction Abort from EL0,
       //// or EC == 0x24 Data Abort from EL0):
       //// When ab_vmar_map() maps a freshly created VMO whose pages have not yet been
       //// committed, it records the VMAR mapping metadata without populating hardware
       //// PTEs. The first time EL0 reads, writes, or fetches an instruction from that
       //// virtual page, the MMU raises EC 0x24 or 0x20 with FAR_EL1 = faulting user VA.
  36 |     /* EC 0x20 (Instruction Abort) / 0x24 (Data Abort) from EL0 -> VMO demand paging! */
  37 |     if (ec == 0x24 || ec == 0x20) {
  38 |         uint32_t fsc = iss & 0x3FU;
  39 |         /* Only demand-page on L1/L2/L3 Translation Faults (FSC 0x05..0x07) */
       //// Filter the 6-bit Fault Status Code (FSC = ISS[5:0]) with '(fsc & 0x3CU) ==
       //// 0x04U':
       //// In ARMv8-A, Translation Faults have binary encodings:
       //// - 0x04 (0b000100): Level 0 Translation Fault
       //// - 0x05 (0b000101): Level 1 Translation Fault
       //// - 0x06 (0b000110): Level 2 Translation Fault
       //// - 0x07 (0b000111): Level 3 Translation Fault
       //// Masking bits [5:2] against 0x04 matches ALL four levels of Translation Faults
       //// while rejecting Permission Faults (0x0D..0x0F, e.g., an illegal write to a
       //// read-only BOOTFS page!).
       //// - Line 41 extracts ISS.WnR (bit 6, Write-not-Read) on Data Aborts so ab_process_handle_page_fault() verifies AB_VM_PERM_WRITE before allocating and mapping a physical frame!
       //// - When ab_process_handle_page_fault() returns 0, returning from arm64_sync_exception() runs regrestore + 'eret' back to the exact faulting ELR_EL1 instruction, which now retries and succeeds transparently!
  40 |         if ((fsc & 0x3CU) == 0x04U) {
  41 |             int is_write = (ec == 0x24 && ((iss >> 6) & 1U)) ? 1 : 0;
  42 |             int is_exec  = (ec == 0x20) ? 1 : 0;
  43 |             ab_process_t *proc = get_current_process();
  44 |             if (proc && ab_process_handle_page_fault(proc, (vaddr_t)far, is_write, is_exec) == 0) {
  45 |                 return;
  46 |             }
  47 |         }
  48 |     }
  49 | 
       //// Fault Isolation Path — Unhandled Exception in EL0 User Space:
       //// If an EL0 process dereferences NULL (0x0), writes to read-only memory
       //// (Permission Fault), executes an undefined instruction, or jumps to an unmapped
       //// address outside any VMAR region, ABOS enforces microkernel fault isolation:
       //// - Logs the crashing process name, KOID, CPU, ESR_EL1, FAR_EL1, and iframe registers.
       //// - Re-enables local interrupts (line 58, required before entering process teardown / scheduler!) and calls ab_process_exit_current(-1), which destroys only the faulting process's handles/address space, signals AB_SIGNAL_TASK_TERMINATED to its supervisor, and switches the CPU to the next runnable thread. The kernel and all other EL0 servers continue running unscathed!
  50 |     /* Isolate EL0 crashes: terminate only the faulting user process, keep kernel alive! */
  51 |     if (exception_flags & ARM64_EXCEPTION_FLAG_LOWER_EL) {
  52 |         ab_process_t *proc = get_current_process();
  53 |         dprintf(CRITICAL, "\n[EXCEPTION] Terminating faulting EL0 process '%s' (koid=%lu) on CPU %u!\n",
  54 |                 proc ? proc->name : "unknown", proc ? proc->base.koid : 0UL, arch_curr_cpu_num());
  55 |         dprintf(CRITICAL, "  ESR=0x%x (EC=0x%x, ISS=0x%x) FAR=%p\n",
  56 |                 esr, ec, iss, (void *)far);
  57 |         dump_iframe(iframe);
  58 |         arch_enable_ints();
  59 |         ab_process_exit_current(-1);
  60 |     }
  61 | 
       //// Terminal Kernel Panic Path — Unhandled Synchronous Exception in EL1 Kernel
       //// Mode:
       //// Unlike EL0 faults, an unhandled exception inside privileged EL1 kernel code
       //// indicates a kernel bug (corrupted pointer or invariant violation). ABOS prints
       //// a [PANIC] diagnostic with ESR_EL1, FAR_EL1, and iframe registers, then halts
       //// the faulting core in an infinite WFE loop.
  62 |     dprintf(CRITICAL, "\n[PANIC] Unhandled EL1 Kernel Sync Exception on CPU %u!\n",
  63 |             arch_curr_cpu_num());
  64 |     dprintf(CRITICAL, "  ESR=0x%x (EC=0x%x, ISS=0x%x) FAR=%p\n",
  65 |             esr, ec, iss, (void *)far);
  66 |     dump_iframe(iframe);
  67 |     for (;;) __asm__ volatile("wfe");
  68 | }
  69 | 
       //// arm64_irq_exception(): Asynchronous hardware interrupt entry point (called
       //// from both .Lel1_irq and .Lel0_irq with PSTATE.I masked by hardware).
  70 | void arm64_irq_exception(iframe_t *iframe, uint32_t exception_flags) {
  71 |     (void)iframe;
  72 |     (void)exception_flags;
  73 | 
       //// Acknowledge the highest-priority pending interrupt from the local CPU's banked
       //// GICv2 CPU Interface:
       //// - Reading GICC_IAR (gic_acknowledge_interrupt) atomically transitions the interrupt from Pending to Active in the GIC state machine and returns the 10-bit Interrupt ID in bits [9:0] (plus source CPUID in bits [12:10] for SGIs).
       //// - Interrupt IDs 1020..1023 (notably 1023 = 0x3FF) indicate a Spurious Interrupt (e.g., level de-asserted or claimed by another core just before IAR read); per the GICv2 spec, spurious IDs require NO EOIR write and return immediately.
  74 |     uint32_t iar = gic_acknowledge_interrupt();
  75 |     uint32_t irq = iar & 0x3FF;
  76 |     if (irq >= 1020) return;
  77 | 
       //// Two-way microkernel IRQ routing:
       //// - IRQ 30 (TIMER_ARCH_PHYS_IRQ, banked per-CPU Private Peripheral Interrupt): Handled inside EL1 by timer_irq_handler() to advance monotonic ticks, wake sleeping/timed-out threads, and set need_resched[cpu] = 1.
       //// - Any other IRQ (such as PL011 UART0 SPI 33): Forwarded to ab_interrupt_dispatch_hw_irq(irq) (defined in kernel/core/object.c), which masks the SPI at the GIC Distributor and signals the bound EL0 driver's InterruptDispatcher event so user-space 'console-srv' wakes up to service the hardware!
  78 |     if (irq == TIMER_ARCH_PHYS_IRQ) {
  79 |         timer_irq_handler();
  80 |     } else {
  81 |         ab_interrupt_dispatch_hw_irq(irq);
  82 |     }
  83 | 
       //// CRITICAL ORDERING INVARIANT — gic_end_interrupt(iar) MUST precede
       //// thread_preempt():
       //// 1. Writing 'iar' to GICC_EOIR drops the local GIC CPU Interface's Running
       //// Priority back to idle so new timer/device interrupts can be delivered on this
       //// CPU.
       //// 2. Only AFTER completing GIC EOI does thread_preempt() check need_resched[cpu]
       //// and potentially context-switch away via thread_yield()! If thread_preempt()
       //// switched away BEFORE writing GICC_EOIR, the GIC CPU interface on this core
       //// would stay stuck at active interrupt priority, deaf to all further timer ticks
       //// until the preempted thread eventually ran again!
  84 |     gic_end_interrupt(iar);
  85 |     thread_preempt();
  86 | }
```
### 8.3.3 Technical Notes

1. **Why Kernel `copy_from_user()` Never Triggers EL1 Demand Paging (`EC == 0x25` vs `0x24`)**:
   Look closely at line 37: `if (ec == 0x24 || ec == 0x20)`. In ARMv8-A, `EC == 0x24` is a Data Abort *from a Lower Exception Level (EL0)*, whereas a Data Abort occurring inside EL1 triggers `EC == 0x25`! Does ABOS ever directly dereference raw user pointers from EL1? **No!** As you will see in Chapter 11 (`syscalls.c`), `copy_from_user()` and `copy_to_user()` translate user virtual addresses in software via `vmm_resolve_vaddr()` (and invoke `ab_process_handle_page_fault()` explicitly if a VMO page has not been faulted in yet!), copying bytes strictly through the kernel's trusted Higher-Half `Physmap`. Thus EL1 never touches `TTBR0_EL1` directly, eliminating `PAN`/`SMAP` bypass hazards by design.

2. **Why `arch_enable_ints()` Is Called Before `ab_process_exit_current(-1)` on Line 58**:
   When an EL0 process faults (say, dereferencing `0x0`), the CPU enters `.Lel0_sync` with `PSTATE.I = 1` (IRQs masked). Inside `ab_process_exit_current(-1)`, the kernel closes all handles of the dying process, marks its threads `THREAD_DEATH`, and calls `thread_exit(-1)` — which never returns to `arm64_sync_exception()`! Calling `arch_enable_ints()` beforehand ensures clean interrupt state during teardown (and `spin_lock_irqsave` inside `thread_exit` -> `sched_reschedule_locked` preserves the exact handoff invariant expected by the next scheduled thread).

---

## 8.4 `kernel/dev/gic.c`

### 8.4.1 Functional Description

ARMv8-A separates interrupt routing from the CPU core into the **Generic Interrupt Controller (GIC)**. QEMU's default `-M virt` platform instantiates a **GICv2** controller split into two MMIO register blocks mapped via ABOS's Higher-Half `Periphmap` (`0xFFFFFFFFC0000000 + PA`):

1. **Distributor (`GICD_*` at `PA 0x08000000` / `VA 0xFFFFFFFFC8000000`)**: Global state shared by all 4 CPUs (except registers `0..31` for SGIs/PPIs, which are banked per accessing CPU). Controls per-interrupt enable/disable bits and target CPU affinity masks.
2. **CPU Interface (`GICC_*` at `PA 0x08010000` / `VA 0xFFFFFFFFC8010000`)**: Hardware-banked per CPU core. Tracks the core's priority mask (`GICC_PMR`), acknowledges the highest-priority pending interrupt (`GICC_IAR`), and completes interrupt servicing (`GICC_EOIR`).

| Interrupt ID Range | GIC Category | Banked per CPU? | Usage in ABOS Lesson 20 |
| :---: | :--- | :---: | :--- |
| `0 .. 15` | **SGI** (Software Generated Interrupt) | Yes | Reserved for inter-processor IPIs |
| `16 .. 31` | **PPI** (Private Peripheral Interrupt) | Yes | **ID 30**: ARMv8 Non-Secure Physical Timer (100 Hz on CPUs 0..3) |
| `32 .. 1019` | **SPI** (Shared Peripheral Interrupt) | No (Global) | **ID 33**: PL011 UART0 RX interrupt routed to EL0 `console-srv` |
| `1020 .. 1023` | **Special / Spurious** | — | **ID 1023**: Spurious interrupt (no pending IRQ at `IAR` read) |

### 8.4.2 Heavily Commented Source Code
#### Program 8-3: `lesson_20/kernel/dev/gic.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/dev/gic.c" start="1" end="48" total="48" -->
```c
       //// kernel/dev/gic.c is a concise 48-line driver for the ARM Generic Interrupt
       //// Controller v2 (GIC-400 on QEMU 'virt'). All MMIO accesses go through the
       //// Higher-Half Periphmap virtual addresses defined in include/gic.h:
       //// - GICD_BASE = 0xFFFFFFFFC8000000 (PA 0x08000000: Global Distributor shared across all 4 CPUs).
       //// - GICC_BASE = 0xFFFFFFFFC8010000 (PA 0x08010000: Banked CPU Interface — every CPU reading/writing this exact same address accesses its own private GICC hardware registers!).
   1 | /* =========================================================================
   2 |  * gic.c - ARM GICv2 Driver Implementation
   3 |  * ========================================================================= */
   4 | #include "gic.h"
   5 | 
       //// gic_dist_init(): Called ONCE by CPU 0 in lk_main() before waking secondary
       //// cores.
       //// 1. Writes 0 to GICD_CTLR (+0x000) to disable interrupt forwarding cleanly,
       //// followed by a full-system Data Synchronization Barrier ('dsb sy').
       //// 2. Writes 1 to GICD_CTLR (EnableGrp0 = 1) to enable forwarding of pending
       //// interrupts from the Distributor to CPU Interfaces, followed by 'dsb sy' so the
       //// enable completes before CPU 0 proceeds.
   6 | void gic_dist_init(void) {
   7 |     GICD_CTLR = 0;
   8 |     __asm__ volatile("dsb sy" ::: "memory");
   9 |     GICD_CTLR = 1;
  10 |     __asm__ volatile("dsb sy" ::: "memory");
  11 | }
  12 | 
       //// gic_cpu_init(): Called on EVERY CPU core (CPU 0 in lk_main(), CPUs 1..3 in
       //// arm64_secondary_entry()).
       //// - GICC_PMR (+0x004, Priority Mask Register) = 0xFF: Sets the priority filter threshold to the lowest possible numerical priority (255), allowing interrupts of any priority 0x00..0xFE to interrupt this core. (Leaving PMR at its reset value 0x00 would block all interrupts!)
       //// - GICC_CTLR (+0x000, CPU Interface Control Register) = 1: Enables signaling of interrupts from this core's CPU interface to the Cortex-A53 IRQ input pin.
  13 | void gic_cpu_init(void) {
  14 |     /* Accept all priority levels (0xFF) and enable Group 0/1 delivery */
  15 |     GICC_PMR  = 0xFF;
  16 |     GICC_CTLR = 1;
  17 |     __asm__ volatile("dsb sy" ::: "memory");
  18 | }
  19 | 
       //// gic_enable_interrupt(uint32_t int_id): Unmasks interrupt ID 'int_id' in the
       //// Distributor.
       //// GICD_ISENABLER (+0x100) is an array of 32-bit Write-1-to-Set registers (1 bit
       //// per interrupt ID: register int_id / 32, bit int_id % 32). Writing a 1 bit
       //// unmasks that interrupt; writing 0 bits has no effect — eliminating any need
       //// for a spinlock around read-modify-write on GICD_ISENABLER! Note also that
       //// GICD_ISENABLER[0] (IDs 0..31, SGIs and PPIs like Timer IRQ 30) is
       //// hardware-banked per CPU, so each CPU calling timer_init_percpu() unmasks its
       //// own private PPI 30.
  20 | void gic_enable_interrupt(uint32_t int_id) {
  21 |     uint32_t reg = int_id / 32;
  22 |     uint32_t bit = int_id % 32;
  23 |     GICD_ISENABLER[reg] = (1U << bit);
  24 | 
       //// Configure CPU routing in GICD_ITARGETSR (+0x800) for Shared Peripheral
       //// Interrupts (SPIs, int_id >= 32, such as UART0 IRQ 33):
       //// - SGIs/PPIs (IDs 0..31) are hardwired to the local CPU, so GICD_ITARGETSR[0..7] are read-only.
       //// - For SPIs (int_id >= 32), each 32-bit GICD_ITARGETSR[int_id / 4] holds four 8-bit CPU target bitmasks. Clearing the byte for (int_id % 4) and setting bit 0 (0x01) routes external SPIs deterministically to CPU 0.
  25 |     /* Route SPIs (ID >= 32) to CPU 0 by default */
  26 |     if (int_id >= 32) {
  27 |         uint32_t target_reg   = int_id / 4;
  28 |         uint32_t target_shift = (int_id % 4) * 8;
  29 |         uint32_t val = GICD_ITARGETSR[target_reg];
  30 |         val &= ~(0xFFU << target_shift);
  31 |         val |=  (0x01U << target_shift);
  32 |         GICD_ITARGETSR[target_reg] = val;
  33 |     }
  34 | }
  35 | 
       //// gic_disable_interrupt(uint32_t int_id): Masks interrupt ID 'int_id' via
       //// GICD_ICENABLER (+0x180, Write-1-to-Clear). Called inside
       //// ab_interrupt_dispatch_hw_irq() immediately when a level-triggered user-space
       //// SPI (like UART0 IRQ 33) fires, preventing an infiniteEL1 interrupt storm while
       //// the EL0 driver ('console-srv') wakes up to drain the hardware FIFO; EL0 later
       //// unmasks it via sys_interrupt_ack() -> gic_enable_interrupt().
  36 | void gic_disable_interrupt(uint32_t int_id) {
  37 |     uint32_t reg = int_id / 32;
  38 |     uint32_t bit = int_id % 32;
  39 |     GICD_ICENABLER[reg] = (1U << bit);
  40 | }
  41 | 
       //// gic_acknowledge_interrupt(): Reads banked GICC_IAR (+0x00C) to claim the
       //// highest-priority pending interrupt on the calling CPU.
  42 | uint32_t gic_acknowledge_interrupt(void) {
  43 |     return GICC_IAR;
  44 | }
  45 | 
       //// gic_end_interrupt(uint32_t iar): Writes the exact 32-bit value originally read
       //// from GICC_IAR into banked GICC_EOIR (+0x010) to signal End-Of-Interrupt
       //// completion (priority drop + deactivation).
  46 | void gic_end_interrupt(uint32_t iar) {
  47 |     GICC_EOIR = iar;
  48 | }
```
### 8.4.3 Technical Notes

1. **Level-Triggered Peripheral SPIs in User-Space Microkernel Drivers**:
   On QEMU `virt`, PL011 UART0 IRQ 33 is **level-sensitive**: its interrupt wire stays asserted as long as unread characters sit in the UART RX FIFO. In a monolithic kernel, the kernel ISR reads `UART_DR` before writing `GICC_EOIR`. In a microkernel where the UART driver lives in unprivileged EL0 (`console-srv`), the kernel *cannot* touch `UART_DR`! Instead, ABOS uses the classic microkernel **Mask-Signal-EOI-Ack** lifecycle:
   - EL1 `ab_interrupt_dispatch_hw_irq(33)` calls `gic_disable_interrupt(33)` (`GICD_ICENABLER`), signals `console-srv`'s `irq_event`, and `arm64_irq_exception` writes `GICC_EOIR`. Because ID 33 is now masked in the Distributor, the CPU does not re-trap!
   - EL0 `console-srv` wakes from `sys_interrupt_wait()`, drains `UART_DR`, clears `UARTICR`, and calls `sys_interrupt_ack()` — which invokes `gic_enable_interrupt(33)` (`GICD_ISENABLER`) to re-arm the interrupt cleanly!

2. **Why `GICC_EOIR` Must Receive the Raw Unmasked `iar` Word**:
   Notice in `exceptions_c.c` (lines 74–84) that `irq = iar & 0x3FF` extracts the 10-bit interrupt number for C logic, yet `gic_end_interrupt(iar)` writes the **entire unmodified 32-bit `iar`** back to `GICC_EOIR`. For SGIs (`ID 0..15`), bits `[12:10]` of `GICC_IAR` identify the originating CPU core, and the GICv2 architecture requires `GICC_EOIR[12:10]` to match `GICC_IAR[12:10]` identically or the SGI remains stuck in the Active state!

---

## 8.5 `kernel/dev/timer.c`

### 8.5.1 Functional Description

Every ARMv8-A core contains an architected **Generic Timer** driven by a system-wide, constant-frequency monotonic counter (`CNTPCT_EL0`) operating at `CNTFRQ_EL0` Hz (`62.5 MHz` on QEMU `virt`). Unlike legacy external PIT/HPET chips, the Generic Timer comparators sit directly inside each CPU core as system registers (`CNTP_TVAL_EL0`, `CNTP_CVAL_EL0`, `CNTP_CTL_EL0`) wired to banked GIC Private Peripheral Interrupt **PPI 30**.

| System Register | Access | Width | Role in `timer.c` |
| :--- | :---: | :---: | :--- |
| `CNTFRQ_EL0` | RO | 64/32-bit | Reports system counter frequency in Hz (`62,500,000` on QEMU `virt`) |
| `CNTPCT_EL0` | RO | 64-bit | Monotonic physical tick count since reset; read by `current_time_ns()` |
| `CNTKCTL_EL1` | RW | 64-bit | Bit 0 (`EL0PCTEN = 1`) grants EL0 direct trap-free read access to `CNTPCT_EL0` |
| `CNTP_TVAL_EL0` | RW | 32-bit signed | Countdown value; writing `N` sets `CVAL = CNTPCT + N` and clears `ISTATUS` |
| `CNTP_CTL_EL0` | RW | 64-bit | Bit 0 (`ENABLE`), Bit 1 (`IMASK`), Bit 2 (`ISTATUS` comparator fired) |

### 8.5.2 Heavily Commented Source Code
#### Program 8-4: `lesson_20/kernel/dev/timer.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/dev/timer.c" start="1" end="51" total="51" -->
```c
       //// kernel/dev/timer.c drives the ARMv8 Generic Timer (Architected Physical Timer)
       //// on every CPU core to provide two essential services:
       //// 1. A periodic 100 Hz (10 ms quantum) per-CPU interrupt (PPI 30) that wakes
       //// sleeping/timed-out threads and triggers preemptive round-robin context
       //// switches.
       //// 2. A lockless, high-resolution monotonic nanosecond clock current_time_ns()
       //// (ab_time_t, modeled after Zircon's zx_clock_get_monotonic()).
   1 | /* =========================================================================
   2 |  * timer.c - ARMv8 Generic Timer Driving the LK Preemptive Scheduler
   3 |  * ========================================================================= */
   4 | #include "timer.h"
   5 | #include "gic.h"
   6 | #include "spinlock.h"
   7 | #include "debuglog.h"
   8 | #include "thread.h"
   9 | 
       //// Module-level timekeeping state:
       //// - timer_freq_hz: Hardware counter frequency read once on CPU 0 from CNTFRQ_EL0 (62,500,000 Hz = 62.5 MHz on QEMU 'virt', i.e., 16 ns per tick).
       //// - timer_interval_cnt: Number of counter ticks per scheduler quantum (62,500,000 / 100 = 625,000 ticks = 10.0 ms).
       //// - cpu_timer_ticks[SMP_MAX_CPUS]: Per-CPU tick counters (cache-separated per CPU index; each CPU increments only its own slot).
  10 | static uint64_t timer_freq_hz = 0;
  11 | static uint64_t timer_interval_cnt = 0;
  12 | static volatile uint64_t cpu_timer_ticks[SMP_MAX_CPUS] = {0};
  13 | 
       //// timer_init_percpu(uint32_t tick_hz): Executed on CPU 0 in lk_main() and on
       //// CPUs 1..3 in arm64_secondary_entry() with tick_hz = 100.
       //// Reads the system counter frequency from CNTFRQ_EL0 (populated by firmware/QEMU
       //// at reset).
  14 | void timer_init_percpu(uint32_t tick_hz) {
  15 |     uint32_t cpu = arch_curr_cpu_num();
  16 |     uint64_t freq;
  17 |     __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  18 | 
       //// CPU 0 records timer_freq_hz and computes timer_interval_cnt = freq / tick_hz
       //// before waking CPUs 1..3 via PSCI, so secondary cores see the initialized
       //// interval immediately.
  19 |     if (cpu == 0) {
  20 |         timer_freq_hz = freq;
  21 |         timer_interval_cnt = freq / tick_hz;
  22 |     }
  23 | 
       //// Program the current CPU's private Generic Timer control registers:
       //// - CNTKCTL_EL1 = 1 (bit 0 = EL0PCTEN): Permits unprivileged EL0 user-space code to execute 'mrs xN, cntpct_el0' and 'mrs xN, cntfrq_el0' directly without trapping to EL1 (the hardware foundation for vDSO clock reads in Zircon!).
       //// - CNTP_TVAL_EL0 = timer_interval_cnt: Loads the 32-bit signed countdown TimerValue register; hardware automatically sets CNTP_CVAL_EL0 = CNTPCT_EL0 + timer_interval_cnt.
       //// - CNTP_CTL_EL0 = 1 (bit 0 ENABLE = 1, bit 1 IMASK = 0): Enables the physical timer comparator and unmasks its interrupt output wire.
       //// - 'isb' ensures the system register writes take effect before unmasking the GIC line.
  24 |     __asm__ volatile("msr cntkctl_el1, %0" :: "r"(1UL)); /* Allow EL0 to read cntpct_el0 */
  25 |     __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval_cnt));
  26 |     __asm__ volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));
  27 |     __asm__ volatile("isb");
  28 | 
       //// Unmask Private Peripheral Interrupt 30 (TIMER_ARCH_PHYS_IRQ = 30, ARM's
       //// Non-Secure EL1 Physical Timer PPI on QEMU 'virt') in the local CPU's banked
       //// GICD_ISENABLER[0].
  29 |     gic_enable_interrupt(TIMER_ARCH_PHYS_IRQ);
  30 | }
  31 | 
       //// timer_irq_handler(): Invoked from arm64_irq_exception() every 10 ms on each
       //// CPU when PPI 30 fires.
       //// - Writing timer_interval_cnt to CNTP_TVAL_EL0 advances CNTP_CVAL_EL0 into the future, which IMMEDIATELY clears CNTP_CTL_EL0.ISTATUS and de-asserts the level-sensitive PPI 30 interrupt wire! (If CNTP_TVAL_EL0 were not reloaded before GICC_EOIR, the level-sensitive PPI 30 line would remain asserted and re-interrupt the CPU endlessly on the very next instruction!)
       //// - Increments cpu_timer_ticks[cpu] and calls thread_timer_tick() in thread.c.
  32 | void timer_irq_handler(void) {
  33 |     uint32_t cpu = arch_curr_cpu_num();
  34 |     __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval_cnt));
  35 |     cpu_timer_ticks[cpu]++;
  36 |     thread_timer_tick();
  37 | }
  38 | 
       //// timer_get_ticks(): Safe bounds-checked accessor returning total timer
       //// interrupts serviced by 'cpu'.
  39 | uint64_t timer_get_ticks(uint32_t cpu) {
  40 |     if (cpu >= SMP_MAX_CPUS) return 0;
  41 |     return cpu_timer_ticks[cpu];
  42 | }
  43 | 
       //// current_time_ns(): Converts the 64-bit monotonic hardware counter CNTPCT_EL0
       //// into nanoseconds since boot WITHOUT overflowing uint64_t!
       //// Why split into quotient (secs = pct / freq) and remainder (rem = pct % freq)
       //// instead of writing '(pct * 1000000000ULL) / timer_freq_hz'?
       //// At 62.5 MHz, naive 'pct * 10^9' overflows 2^64 (1.84 * 10^19) after just 295
       //// seconds (~4.9 minutes) — and on a 1 GHz counter it overflows in 18.4 seconds!
       //// By splitting secs and rem (where rem < freq, so rem * 10^9 < 6.25 * 10^16 <<
       //// 2^64), current_time_ns() remains exact and overflow-free for 584 years!
  44 | ab_time_t current_time_ns(void) {
  45 |     uint64_t pct;
  46 |     __asm__ volatile("mrs %0, cntpct_el0" : "=r"(pct));
  47 |     if (timer_freq_hz == 0) return 0;
  48 |     uint64_t secs = pct / timer_freq_hz;
  49 |     uint64_t rem  = pct % timer_freq_hz;
  50 |     return secs * 1000000000ULL + (rem * 1000000000ULL) / timer_freq_hz;
  51 | }
```
### 8.5.3 Technical Notes

1. **Countdown `CNTP_TVAL_EL0` vs Absolute `CNTP_CVAL_EL0` Drift Behavior**:
   Writing `CNTP_TVAL_EL0 = timer_interval_cnt` inside `timer_irq_handler()` schedules the next interrupt at `CNTPCT_EL0_now + timer_interval_cnt` (`10 ms` from when the handler ran). Because ABOS measures all wall-clock sleeps (`thread_sleep_ns`, `sys_nanosleep`, `event_wait_deadline`) against the hardware free-running counter `current_time_ns()` (`CNTPCT_EL0`) rather than counting `cpu_timer_ticks`, small IRQ delivery latencies in `CNTP_TVAL_EL0` never accumulate into monotonic clock drift!

2. **Independent Per-CPU Tick Streams in 4-Core SMP**:
   Because `timer_init_percpu(100)` runs on all four CPUs (`0..3`), all four cores receive independent 100 Hz PPI 30 interrupts. Even if CPU 0 is busy running a compute-bound EL0 task (`calc.bin`) while CPUs 1..3 run shells and servers, every CPU independently preempts its own running thread every 10 ms (`need_resched[cpu] = 1`).

---

## 8.6 `kernel/core/switch.S`

### 8.6.1 Functional Description

`kernel/core/switch.S` is the 42-line heart of thread context switching in ABOS. Called exclusively from `sched_reschedule_locked()` (`thread.c` line 242) with `sched_lock` held and local interrupts disabled, `arm64_context_switch(vaddr_t *old_sp, vaddr_t new_sp)` freezes the calling kernel execution stream onto its current stack, saves `SP` into `prev->kernel_sp`, loads `next->kernel_sp` into `SP`, restores `next`'s non-volatile state, and executes `ret` to resume `next`.

| Stack Offset | Register Pair | `context_switch_frame_t` Field | Purpose |
| :---: | :--- | :--- | :--- |
| `[sp, #0]` | `x9, x10` (`tpidr_el0`, `tpidrro_el0`) | `tpidr_el0`, `tpidrro_el0` | Per-thread EL0 TLS base pointers |
| `[sp, #16]` | `x19, x20` | `r19`, `r20` | AAPCS64 callee-saved GPRs |
| `[sp, #32]` | `x21, x22` | `r21`, `r22` | AAPCS64 callee-saved GPRs |
| `[sp, #48]` | `x23, x24` | `r23`, `r24` | AAPCS64 callee-saved GPRs |
| `[sp, #64]` | `x25, x26` | `r25`, `r26` | AAPCS64 callee-saved GPRs |
| `[sp, #80]` | `x27, x28` | `r27`, `r28` | AAPCS64 callee-saved GPRs |
| `[sp, #96]` | `x29, x30` | `r29` (FP), `lr` (Return PC) | Frame pointer & resume PC (`112 B` total) |

### 8.6.2 Heavily Commented Source Code
#### Program 8-5: `lesson_20/kernel/core/switch.S`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/switch.S" start="1" end="42" total="42" -->
```asm
       ## kernel/core/switch.S implements arm64_context_switch(vaddr_t *old_sp, vaddr_t
       ## new_sp), modeled directly after Zircon's zircon/kernel/arch/arm64/asm.S.
       ## Unlike exceptions.S (which saves ALL 31 general-purpose registers because an
       ## asynchronous IRQ or trap can strike at any instruction), arm64_context_switch
       ## is a synchronous C-callable function invoked exclusively from
       ## sched_reschedule_locked() in thread.c.
   1 | /* =========================================================================
   2 |  * switch.S - Zircon-Style AArch64 Context Switch (arm64_context_switch)
   3 |  * References:
   4 |  *   - zircon_org/kernel/arch/arm64/asm.S (arm64_context_switch)
   5 |  * ========================================================================= */
   6 | 
   7 | .section .text
   8 | .global arm64_context_switch
   9 | 
       ## C prototype: void arm64_context_switch(vaddr_t *old_sp /* x0 */, vaddr_t
       ## new_sp /* x1 */);
       ## Per the ARM64 Procedure Call Standard (AAPCS64):
       ## - Registers x0..x18 are caller-saved (volatile): the C compiler already spilled any live values in x0..x18 onto the caller's stack frame before emitting 'bl arm64_context_switch'.
       ## - Registers x19..x28, x29 (Frame Pointer), and x30 (Link Register / return PC) are callee-saved (non-volatile) and MUST be preserved across the call.
  10 | /* void arm64_context_switch(vaddr_t *old_sp, vaddr_t new_sp); */
  11 | arm64_context_switch:
  12 |     /* Save callee-saved registers + tpidr_el0/tpidrro_el0 (14 * 8 = 112 bytes) */
       ## Carve a 112-byte (14 * 8 bytes = 7 * 16-byte pairs) context_switch_frame_t on
       ## the outgoing thread's EL1 kernel stack and save the two EL0 Thread-Local
       ## Storage (TLS) system registers:
       ## - tpidr_el0   (read/write EL0 thread pointer used by C runtimes / pthreads).
       ## - tpidrro_el0 (read-only EL0 thread pointer).
       ## Saving these two system registers here ensures per-thread TLS isolation across
       ## user threads without bloating every IRQ's iframe_t.
  13 |     sub     sp, sp, #112
  14 |     mrs     x9,  tpidr_el0
  15 |     mrs     x10, tpidrro_el0
  16 |     stp     x9,  x10, [sp, #(16 * 0)]
       ## Save callee-saved general-purpose registers x19..x28, frame pointer x29, and
       ## return address x30 (LR) into offsets 16..111 of context_switch_frame_t. Note
       ## how exact offsets [sp, #0] .. [sp, #96] match the C struct fields (tpidr_el0,
       ## tpidrro_el0, r19..r29, lr) of context_switch_frame_t in include/thread.h
       ## byte-for-byte!
  17 |     stp     x19, x20, [sp, #(16 * 1)]
  18 |     stp     x21, x22, [sp, #(16 * 2)]
  19 |     stp     x23, x24, [sp, #(16 * 3)]
  20 |     stp     x25, x26, [sp, #(16 * 4)]
  21 |     stp     x27, x28, [sp, #(16 * 5)]
  22 |     stp     x29, x30, [sp, #(16 * 6)]
  23 | 
       ## Snapshot the outgoing thread's current EL1 stack pointer (sp) into *old_sp
       ## (i.e., prev->kernel_sp). At this exact instruction, the outgoing thread's
       ## complete execution state is frozen cleanly on its own 8 KB kernel stack.
  24 |     /* Store old stack pointer into *old_sp (x0) */
  25 |     mov     x9, sp
  26 |     str     x9, [x0]
  27 | 
       ## THE STACK PIVOT ('mov sp, x1'):
       ## In a single cycle, replacing SP with new_sp (next->kernel_sp) switches
       ## execution from the outgoing thread's kernel stack to the incoming thread's
       ## kernel stack! From line 32 onward, we are restoring the incoming thread.
  28 |     /* Load new stack pointer from new_sp (x1) */
  29 |     mov     sp, x1
  30 | 
       ## Pop tpidr_el0, tpidrro_el0, x19..x29, and x30 (LR) from the incoming thread's
       ## context_switch_frame_t:
       ## - If 'next' was previously preempted or blocked inside sched_reschedule_locked(), its saved x30 points right back to line 243 of thread.c (the instruction after 'bl arm64_context_switch').
       ## - If 'next' is a brand-new thread created by thread_create(), thread_create() seeded frame->lr = initial_thread_trampoline (line 153 of thread.c)!
  31 |     /* Restore callee-saved registers + tpidr_el0/tpidrro_el0 */
  32 |     ldp     x9,  x10, [sp, #(16 * 0)]
  33 |     msr     tpidr_el0,   x9
  34 |     msr     tpidrro_el0, x10
  35 |     ldp     x19, x20, [sp, #(16 * 1)]
  36 |     ldp     x21, x22, [sp, #(16 * 2)]
  37 |     ldp     x23, x24, [sp, #(16 * 3)]
  38 |     ldp     x25, x26, [sp, #(16 * 4)]
  39 |     ldp     x27, x28, [sp, #(16 * 5)]
  40 |     ldp     x29, x30, [sp, #(16 * 6)]
       ## Deallocate the 112-byte context_switch_frame_t ('add sp, sp, #112') and
       ## execute 'ret' (branching to x30), seamlessly resuming the incoming thread!
  41 |     add     sp, sp, #112
  42 |     ret
```
### 8.6.3 Technical Notes

1. **Why `arm64_context_switch` Does Not Need an Explicit `dsb` or `isb` Instruction**:
   When switching between two threads in the **same** address space (or two kernel threads), both stacks live in Normal Inner-Shareable WBWA memory (`Physmap`), and `sched_lock`'s `ldaxr` / `stlr` instructions in the caller already provide full acquire-release SMP memory ordering across cores! When switching to a thread in a **different** user `VmAspace`, `sched_reschedule_locked()` calls `vmm_switch_aspace(next->aspace)` immediately beforehand (line 239 of `thread.c`), which executes `msr ttbr0_el1, %0` followed by `isb`.

---

## 8.7 `kernel/core/uspace_entry.S`

### 8.7.1 Functional Description

How does a brand-new user process (`userboot`, `console-srv`, `fs-srv`, `shell`, or `calc`) execute its very first EL0 instruction when it has never trapped *into* the kernel before (and therefore has no `iframe_t` on its stack)?

When `ab_process_start_user()` (`object.c`) starts a user thread, it creates a kernel thread whose entry routine is `user_thread_trampoline()`. That C function switches `TTBR0_EL1` to the target process's `VmAspace`, allocates and maps the 24 KB EL0 user stack `[0x00800000 .. 0x00806000)`, and calls `arm64_uspace_entry(arg0, arg1, user_entry, user_sp, kstack_top)` in `kernel/core/uspace_entry.S`. `arm64_uspace_entry` rewinds `SP_EL1` to `kstack_top`, loads `SP_EL0`, `ELR_EL1`, and `SPSR_EL1 = 0`, zeroes `x2..x30`, and executes `eret` to enter EL0 cleanly.

### 8.7.2 Heavily Commented Source Code
#### Program 8-6: `lesson_20/kernel/core/uspace_entry.S`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/uspace_entry.S" start="1" end="50" total="50" -->
```asm
       ## kernel/core/uspace_entry.S implements the one-way privilege transition from
       ## EL1 kernel mode down into unprivileged EL0 user mode when starting a new user
       ## thread (called from user_thread_trampoline() in kernel/core/object.c).
   1 | /* =========================================================================
   2 |  * uspace_entry.S - Enter EL0 User Space from Kernel Thread
   3 |  * Reference: zircon_org/kernel/arch/arm64/uspace_entry.S
   4 |  * ========================================================================= */
   5 | 
   6 | .section .text
   7 | .global arm64_uspace_entry
   8 | 
       ## Five AAPCS64 register arguments passed from user_thread_trampoline():
       ## - x0 = arg1: Initial first argument to EL0 _start / userboot_entry (e.g., bootstrap channel handle ch1_h or processargs channel handle!).
       ## - x1 = arg2: Initial second argument (0 in Lesson 20, reserved for stack/vDSO cookie).
       ## - x2 = pc: User-space virtual entry address (USER_KVA_TO_UVA(userboot_entry) for userboot, or 0x20000000 for standalone servers/apps).
       ## - x3 = user_sp: Top of the newly mapped 24 KB EL0 user stack (0x00806000).
       ## - x4 = kstack_top: Top of this thread's 8 KB EL1 kernel stack ((stack_base + 8192) & ~15).
   9 | /* void arm64_uspace_entry(uintptr_t arg1, uintptr_t arg2, uintptr_t pc,
  10 |  *                         uintptr_t user_sp, vaddr_t kstack_top);
  11 |  */
  12 | arm64_uspace_entry:
       ## Reset the current EL1 stack pointer (SP_EL1) all the way back to kstack_top!
       ## Because user_thread_trampoline() will NEVER be returned to, discarding its C
       ## stack frames reclaims 100% of the 8 KB kernel stack for future EL0->EL1
       ## syscalls and IRQs taken by this user thread.
  13 |     mov     sp, x4              /* Set EL1 kernel stack pointer */
       ## Program the user-mode stack pointer (SP_EL0 <- x3) and exception return PC
       ## (ELR_EL1 <- x2).
  14 |     msr     sp_el0, x3          /* Set EL0 user stack pointer */
  15 |     msr     elr_el1, x2         /* Set EL0 entry PC */
       ## Construct SPSR_EL1 = 0x0 and clear MDSCR_EL1:
       ## Why is SPSR_EL1 = 0 the exact encoding we want for user mode?
       ## - Bits [3:0] (M[3:0]) = 0b0000: Selects AArch64 Exception Level 0 using SP_EL0 (EL0t)!
       ## - Bit 4 (M[4]) = 0: AArch64 execution state (not AArch32).
       ## - Bits [9:6] (D, A, I, F) = 0b0000: Unmasks Debug, SError, IRQ, and FIQ exceptions atomically the instant 'eret' lands in EL0!
       ## - Bits [31:28] (NZCV) = 0b0000: Clean condition flags.
  16 |     mov     x9, #0              /* SPSR_EL1 = 0 (EL0t, IRQs enabled) */
  17 |     msr     spsr_el1, x9
  18 |     msr     mdscr_el1, xzr
  19 | 
       ## Security Hygiene — Scrub every general-purpose register from x2 through x30 to
       ## zero (xzr), leaving ONLY intentional ABI arguments x0 (arg1) and x1 (arg2)
       ## intact!
       ## Without lines 21-49, residual Higher-Half kernel pointers (0xFFFFFFFF80xxxxxx)
       ## sitting in caller/callee registers from user_thread_trampoline() would leak
       ## directly into untrusted EL0 registers at process startup (an
       ## information-disclosure vulnerability class mitigated identically in Zircon's
       ## arm64_uspace_entry).
  20 |     /* Zero all general-purpose registers (except arg1=x0, arg2=x1) to avoid leaking kernel state */
  21 |     mov     x2,  xzr
  22 |     mov     x3,  xzr
  23 |     mov     x4,  xzr
  24 |     mov     x5,  xzr
  25 |     mov     x6,  xzr
  26 |     mov     x7,  xzr
  27 |     mov     x8,  xzr
  28 |     mov     x9,  xzr
  29 |     mov     x10, xzr
  30 |     mov     x11, xzr
  31 |     mov     x12, xzr
  32 |     mov     x13, xzr
  33 |     mov     x14, xzr
  34 |     mov     x15, xzr
  35 |     mov     x16, xzr
  36 |     mov     x17, xzr
  37 |     mov     x18, xzr
  38 |     mov     x19, xzr
  39 |     mov     x20, xzr
  40 |     mov     x21, xzr
  41 |     mov     x22, xzr
  42 |     mov     x23, xzr
  43 |     mov     x24, xzr
  44 |     mov     x25, xzr
  45 |     mov     x26, xzr
  46 |     mov     x27, xzr
  47 |     mov     x28, xzr
  48 |     mov     x29, xzr
  49 |     mov     x30, xzr
       ## Execute 'eret': Hardware atomically drops privilege from EL1h to EL0t,
       ## switches the active stack pointer selector from SP_EL1 to SP_EL0 (0x00806000),
       ## unmasks interrupts (SPSR_EL1 = 0), and fetches the first user instruction at
       ## PC = ELR_EL1!
  50 |     eret
```
### 8.7.3 Technical Notes

1. **Why `SPSR_EL1 = 0` Automatically Enables Interrupts Upon Reaching EL0**:
   Notice that `user_thread_trampoline()` runs in EL1 with interrupts enabled (from `initial_thread_trampoline()`), and even if interrupts were masked prior to `arm64_uspace_entry`, writing `0` to `SPSR_EL1` clears bits `[9:6]` (`DAIF = 0b0000`). In ARMv8-A, EL0 cannot execute `msr daifset`/`daifclr` (doing so traps unless `SCTLR_EL1.UMA` is set, which ABOS leaves `0` so untrusted EL0 code can **never** disable hardware interrupts or starve the 100 Hz preemptive timer!).

---

## 8.8 `kernel/core/thread.c`

### 8.8.1 Functional Description

`kernel/core/thread.c` is the SMP scheduling engine of ABOS. Every thread in the system — whether a pure EL1 kernel worker or the backing execution context (`uthr->kthread`) of an EL0 user thread — is represented by a `thread_t` structure (defined in `include/thread.h`) moving through six well-defined states:

```text
                       thread_create()
                             |
                             v
                     +----------------+
                     | THREAD_INITIAL |
                     +----------------+
                             | thread_resume()
                             v
            +--------> +--------------+ <-------------------------+
            |          | THREAD_READY |                           |
  Preempt / |          +--------------+                           | event_signal() /
  Yield     |                | sched_reschedule_locked()          | timer wakeup /
            |                v                                    | timeout
            |         +----------------+                          |
            +-------- | THREAD_RUNNING | -------------------------+
                      +----------------+    thread_sleep_ns() /
                             |              wait_queue_block_locked()
               thread_exit() |              -> THREAD_SLEEPING / THREAD_BLOCKED
                             v
                     +----------------+
                     |  THREAD_DEATH  | ---> thread_reap_dead() frees 8KB stack & struct
                     +----------------+      (once detached == 1 && !on_cpu across CPUs 0..3)
```

### 8.8.2 Heavily Commented Source Code
#### Program 8-7: `lesson_20/kernel/core/thread.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/thread.c" start="1" end="317" total="317" -->
```c
       //// kernel/core/thread.c implements ABOS's symmetric multiprocessing (SMP)
       //// preemptive round-robin thread scheduler, modeled after Little Kernel (LK) and
       //// Zircon's thread/scheduler core. All 4 Cortex-A53 CPUs share a single global
       //// thread table protected by an IRQ-safe spinlock (sched_lock), allowing unpinned
       //// kernel and EL0 user threads to migrate fluidly across idle cores while
       //// supporting per-CPU pinning for idle threads and real-time tests.
   1 | /* =========================================================================
   2 |  * thread.c - LK Threading & SMP Preemptive Round-Robin Scheduler
   3 |  * References:
   4 |  *   - zircon_org/kernel/kernel/thread.cpp
   5 |  *   - zircon_org/kernel/kernel/sched.cpp
   6 |  *   - zircon_org/kernel/arch/arm64/thread.cpp
   7 |  * ========================================================================= */
   8 | #include "thread.h"
   9 | #include "wait.h"
  10 | #include "spinlock.h"
  11 | #include "heap.h"
  12 | #include "pmm.h"
  13 | #include "debuglog.h"
  14 | 
       //// Global SMP scheduler state (zero-initialized in .bss, configured in
       //// thread_init_early()):
       //// - sched_lock: Single fine-grained IRQ-disabling spinlock serializing all thread state transitions, wait_queue operations, and thread_table updates across CPUs 0..3.
       //// - thread_table[MAX_THREADS]: Flat table of up to 64 active non-idle kernel/user threads.
       //// - current_threads[SMP_MAX_CPUS]: Tracks the thread_t currently executing on each CPU (0..3).
       //// - idle_threads[SMP_MAX_CPUS]: Statically allocated per-CPU idle thread descriptors (tid = 0) whose stacks are the 8 KB boot stacks carved in linker.ld!
       //// - need_resched[SMP_MAX_CPUS]: Per-CPU preemption flag set by thread_timer_tick() (in IRQ context) and consumed by thread_preempt() just before returning from arm64_irq_exception().
       //// - next_tid: Monotonically increasing kernel thread ID counter starting at 1.
  15 | static spin_lock_t sched_lock = SPIN_LOCK_INITIAL_VALUE;
  16 | static thread_t *thread_table[MAX_THREADS];
  17 | static thread_t *current_threads[SMP_MAX_CPUS];
  18 | static thread_t  idle_threads[SMP_MAX_CPUS];
  19 | static volatile int need_resched[SMP_MAX_CPUS];
  20 | static uint32_t next_tid = 1;
  21 | 
       //// copy_name(): Safe bounded string copy guaranteeing NUL termination inside
       //// thread_t.name[32] without pulling in libc strncpy/strlcpy.
  22 | static void copy_name(char *dst, const char *src, size_t max) {
  23 |     size_t i = 0;
  24 |     while (src && src[i] && i + 1 < max) {
  25 |         dst[i] = src[i];
  26 |         i++;
  27 |     }
  28 |     dst[i] = '\0';
  29 | }
  30 | 
       //// sched_lock_acquire() and sched_lock_release(): Exported wrappers around
       //// spin_lock_irqsave(&sched_lock, ...) and spin_lock_irqrestore(&sched_lock, ...)
       //// used by wait_queue_t and event_t in wait.c so thread blocking and waking
       //// execute atomically with scheduler state transitions.
  31 | void sched_lock_acquire(uint64_t *irqstate_out) {
  32 |     spin_lock_irqsave(&sched_lock, irqstate_out);
  33 | }
  34 | 
  35 | void sched_lock_release(uint64_t irqstate) {
  36 |     spin_lock_irqrestore(&sched_lock, irqstate);
  37 | }
  38 | 
       //// get_current_thread(): Returns the thread_t running on the calling CPU via
       //// MPIDR_EL1 & 0xFF. Note that callers running with interrupts enabled must be
       //// aware they could migrate across CPUs unless pinned or holding an IRQ-saving
       //// spinlock; inside syscalls and traps, current_threads[cpu] always identifies
       //// the calling thread because migration only happens when the thread itself
       //// enters sched_reschedule_locked()!
  39 | thread_t *get_current_thread(void) {
  40 |     return current_threads[arch_curr_cpu_num()];
  41 | }
  42 | 
       //// initial_thread_trampoline(): The synthetic birth routine of every thread
       //// created by thread_create()!
       //// STUDY LINES 45-46 CAREFULLY — this is one of the most famous invariants in OS
       //// kernel design:
       ////   arch_spin_unlock(&sched_lock);
       ////   arch_enable_ints();
       //// Why does a brand-new thread unlock sched_lock when it never called
       //// spin_lock_irqsave(&sched_lock)?
       //// Because the PREVIOUS thread on this CPU acquired sched_lock (with IRQs masked)
       //// inside thread_yield() / wait_queue_block_locked() BEFORE calling
       //// sched_reschedule_locked() -> arm64_context_switch()! When arm64_context_switch
       //// pivoted SP to this new thread's synthetic frame and executed 'ret', control
       //// jumped straight to initial_thread_trampoline() instead of returning to
       //// sched_reschedule_locked()! Releasing sched_lock and enabling IRQs here
       //// completes the cross-thread lock handoff cleanly before invoking
       //// cur->entry(cur->arg) and terminating via thread_exit(ret).
  43 | static void initial_thread_trampoline(void) {
  44 |     /* Release sched_lock held across arm64_context_switch and enable IRQs */
  45 |     arch_spin_unlock(&sched_lock);
  46 |     arch_enable_ints();
  47 | 
  48 |     thread_t *cur = get_current_thread();
  49 |     int ret = 0;
  50 |     if (cur && cur->entry) {
  51 |         ret = cur->entry(cur->arg);
  52 |     }
  53 |     thread_exit(ret);
  54 | }
  55 | 
       //// thread_init_early(): Called at line 52 of lk_main() on CPU 0 before secondary
       //// CPUs wake.
       //// Clears thread_table[] and initializes idle_threads[0..3]:
       //// - Notice idle_threads[] are NOT placed into thread_table[]! Instead, sched_reschedule_locked() selects &idle_threads[cpu] explicitly as the per-CPU fallback whenever no THREAD_READY thread in thread_table[] can run on 'cpu'.
       //// - Setting idle->aspace = NULL ensures switching to an idle thread points TTBR0_EL1 at empty_ttbr0_l1.
  56 | void thread_init_early(void) {
  57 |     for (int i = 0; i < MAX_THREADS; i++) {
  58 |         thread_table[i] = NULL;
  59 |     }
  60 |     for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
  61 |         thread_t *idle = &idle_threads[cpu];
  62 |         idle->tid        = 0;
  63 |         copy_name(idle->name, "idle", sizeof(idle->name));
  64 |         idle->state      = THREAD_RUNNING;
  65 |         idle->pinned_cpu = (int)cpu;
  66 |         idle->aspace     = NULL;
  67 |         current_threads[cpu] = idle;
  68 |         need_resched[cpu]    = 0;
  69 |     }
  70 | }
  71 | 
       //// thread_secondary_cpu_init(): Ensures current_threads[cpu] points to
       //// &idle_threads[cpu] when a secondary CPU enters arm64_secondary_entry().
  72 | void thread_secondary_cpu_init(void) {
  73 |     uint32_t cpu = arch_curr_cpu_num();
  74 |     current_threads[cpu] = &idle_threads[cpu];
  75 | }
  76 | 
       //// thread_detach(): Marks a thread eligible for automatic kernel stack and struct
       //// reclamation once it enters THREAD_DEATH.
       //// - Pure kernel threads start with detached = 1 (line 143).
       //// - User threads created by ab_process_start_user() start with detached = 0 (owned by their ab_thread_dispatcher_t) so the dispatcher can inspect thread state until all handles to the Thread/Process close; thread_dispatcher_destroy() then calls thread_detach(td->kthread) to sever process_obj/thread_obj pointers and set detached = 1!
  77 | void thread_detach(thread_t *t) {
  78 |     if (!t) return;
  79 |     uint64_t irqstate;
  80 |     spin_lock_irqsave(&sched_lock, &irqstate);
  81 |     t->thread_obj  = NULL;
  82 |     t->process_obj = NULL;
  83 |     t->detached    = 1;
  84 |     spin_lock_irqrestore(&sched_lock, irqstate);
  85 | }
  86 | 
       //// thread_reap_dead(): Deferred garbage collector for exited detached threads,
       //// invoked from every CPU's idle loop (line 313) and opportunistically at the top
       //// of thread_create() (line 123).
  87 | void thread_reap_dead(void) {
  88 |     for (;;) {
  89 |         thread_t *victim = NULL;
  90 |         uint64_t irqstate;
  91 |         spin_lock_irqsave(&sched_lock, &irqstate);
  92 | 
  93 |         for (int i = 0; i < MAX_THREADS; i++) {
  94 |             thread_t *t = thread_table[i];
  95 |             if (!t || t->state != THREAD_DEATH || !t->detached) continue;
  96 | 
       //// SUBTLE 4-CORE SMP SAFETY CHECK ('on_cpu' scan across current_threads[0..3]):
       //// Suppose Thread T on CPU 2 calls thread_exit() (line 270): under sched_lock, T
       //// sets cur->state = THREAD_DEATH and calls sched_reschedule_locked(). Inside
       //// sched_reschedule_locked(), CPU 2 updates current_threads[2] = next BEFORE
       //// calling arm64_context_switch(), while STILL holding sched_lock until 'next'
       //// releases sched_lock after switching stacks!
       //// Even so, verifying 'current_threads[c] != t' across all 4 CPUs under
       //// sched_lock guarantees that 't' is neither designated as any CPU's active
       //// thread nor executing on its kernel stack — making it 100% safe to free
       //// victim->stack_base!
  97 |             int on_cpu = 0;
  98 |             for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
  99 |                 if (current_threads[c] == t) {
 100 |                     on_cpu = 1;
 101 |                     break;
 102 |                 }
 103 |             }
 104 |             if (!on_cpu) {
 105 |                 thread_table[i] = NULL;
 106 |                 victim = t;
 107 |                 break;
 108 |             }
 109 |         }
 110 | 
       //// Lock-Order Discipline: Drop sched_lock BEFORE calling pmm_free_page() and
       //// kfree(victim)!
       //// Acquiring pmm_lock or heap_lock while holding sched_lock would lengthen
       //// IRQ-disabled scheduler critical sections across all 4 CPUs and risk AB-BA lock
       //// inversions. By unlinking one 'victim' from thread_table[i] = NULL under
       //// sched_lock and freeing its 8 KB physical stack + heap struct outside the lock
       //// in a loop, reclamation is O(1)-lock-hold-time and deadlock-free.
 111 |         spin_lock_irqrestore(&sched_lock, irqstate);
 112 |         if (!victim) break;
 113 | 
 114 |         paddr_t stack_pa = physmap_to_paddr((const void *)victim->stack_base);
 115 |         for (size_t off = 0; off < victim->stack_size; off += PAGE_SIZE) {
 116 |             pmm_free_page(stack_pa + off);
 117 |         }
 118 |         kfree(victim);
 119 |     }
 120 | }
 121 | 
       //// thread_create(): Allocates a new kernel thread control block and an 8 KB
       //// (THREAD_STACK_SIZE = 2 * 4096 B) physically contiguous kernel stack from PMM
       //// (tagged VM_PAGE_STATE_ALLOC) mapped via Higher-Half Physmap.
 122 | thread_t *thread_create(const char *name, thread_start_routine entry, void *arg) {
 123 |     thread_reap_dead();
 124 | 
 125 |     thread_t *t = (thread_t *)kzalloc(sizeof(thread_t));
 126 |     if (!t) return NULL;
 127 | 
 128 |     paddr_t stack_pa = 0;
 129 |     if (!pmm_alloc_contiguous(THREAD_STACK_SIZE / PAGE_SIZE, VM_PAGE_STATE_ALLOC, &stack_pa)) {
 130 |         kfree(t);
 131 |         return NULL;
 132 |     }
 133 | 
       //// Initialize thread metadata:
       //// - state = THREAD_INITIAL: The thread will NOT be scheduled by any CPU until its creator explicitly calls thread_resume(t).
       //// - pinned_cpu = -1: Eligible to run on any of CPUs 0..3 by default.
       //// - detached = 1: Reaped automatically on exit unless overridden (as ab_process_start_user does for user threads).
 134 |     t->stack_base  = (vaddr_t)paddr_to_physmap(stack_pa);
 135 |     t->stack_size  = THREAD_STACK_SIZE;
 136 |     t->entry       = entry;
 137 |     t->arg         = arg;
 138 |     t->state       = THREAD_INITIAL;
 139 |     t->pinned_cpu  = -1;
 140 |     t->aspace      = NULL;
 141 |     t->blocking_wq = NULL;
 142 |     t->wait_next   = NULL;
 143 |     t->detached    = 1;
 144 |     copy_name(t->name, name ? name : "kthread", sizeof(t->name));
 145 | 
       //// Forge the initial 112-byte context_switch_frame_t at the top of the 8 KB
       //// stack:
       ////   +-----------------------------------------+ <- stack_base + 8192 (sp_top, 16B aligned)
       ////   | context_switch_frame_t (112 bytes)      |
       ////   |   tpidr_el0 = 0, tpidrro_el0 = 0        |
       ////   |   r19 .. r29 = 0                        |
       ////   |   lr = initial_thread_trampoline        |
       ////   +-----------------------------------------+ <- t->kernel_sp = sp_top - 112
       ////   |   (~8080 bytes of free stack downward)  |
       ////   +-----------------------------------------+ <- stack_base
       //// Zeroing the frame and setting frame->lr = initial_thread_trampoline makes a
       //// brand-new thread indistinguishable to arm64_context_switch() from an existing
       //// suspended thread!
 146 |     /* Set up initial context_switch_frame_t at the top of the 8KB stack */
 147 |     vaddr_t sp_top = (t->stack_base + t->stack_size) & ~15ULL;
 148 |     context_switch_frame_t *frame =
 149 |         (context_switch_frame_t *)(sp_top - sizeof(context_switch_frame_t));
 150 |     for (size_t i = 0; i < sizeof(*frame) / sizeof(uint64_t); i++) {
 151 |         ((uint64_t *)frame)[i] = 0;
 152 |     }
 153 |     frame->lr = (uint64_t)(uintptr_t)initial_thread_trampoline;
 154 |     t->kernel_sp = (vaddr_t)frame;
 155 | 
       //// Register the thread in thread_table[] under sched_lock. If all 64 slots are
       //// occupied (!placed), cleanly roll back both the 2 PMM stack pages and the
       //// kzalloc'd thread_t so no memory leaks.
 156 |     uint64_t irqstate;
 157 |     spin_lock_irqsave(&sched_lock, &irqstate);
 158 |     t->tid = next_tid++;
 159 |     int placed = 0;
 160 |     for (int i = 0; i < MAX_THREADS; i++) {
 161 |         if (thread_table[i] == NULL) {
 162 |             thread_table[i] = t;
 163 |             placed = 1;
 164 |             break;
 165 |         }
 166 |     }
 167 |     spin_lock_irqrestore(&sched_lock, irqstate);
 168 | 
 169 |     if (!placed) {
 170 |         for (size_t off = 0; off < THREAD_STACK_SIZE; off += PAGE_SIZE) {
 171 |             pmm_free_page(stack_pa + off);
 172 |         }
 173 |         kfree(t);
 174 |         return NULL;
 175 |     }
 176 |     return t;
 177 | }
 178 | 
       //// thread_set_pinned_cpu() & thread_resume():
       //// thread_resume() transitions a thread from INITIAL, BLOCKED, or SLEEPING to
       //// THREAD_READY under sched_lock, then executes 'sev' (Send Event) to wake any
       //// idle cores sleeping in 'wfe'/'wfi' so an idle secondary CPU can pick up the
       //// newly runnable thread immediately rather than waiting up to 10 ms for its next
       //// timer tick!
 179 | void thread_set_pinned_cpu(thread_t *t, int cpu) {
 180 |     if (t) t->pinned_cpu = cpu;
 181 | }
 182 | 
 183 | void thread_resume(thread_t *t) {
 184 |     if (!t) return;
 185 |     uint64_t irqstate;
 186 |     spin_lock_irqsave(&sched_lock, &irqstate);
 187 |     if (t->state == THREAD_INITIAL || t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
 188 |         t->state = THREAD_READY;
 189 |     }
 190 |     spin_lock_irqrestore(&sched_lock, irqstate);
 191 |     __asm__ volatile("sev");
 192 | }
 193 | 
 194 | /* Must be called with sched_lock held and IRQs disabled */
       //// sched_reschedule_locked(): Core SMP Round-Robin scheduling algorithm (caller
       //// MUST hold sched_lock with local IRQs disabled!).
       //// First, locate 'prev' inside thread_table[] and set start_idx = (i + 1) %
       //// MAX_THREADS. Starting the search one slot AFTER the outgoing thread guarantees
       //// strict O(MAX_THREADS) round-robin fairness so low-index slots never starve
       //// higher-index slots!
 195 | void sched_reschedule_locked(void) {
 196 |     uint32_t cpu = arch_curr_cpu_num();
 197 |     thread_t *prev = current_threads[cpu];
 198 |     thread_t *next = NULL;
 199 | 
 200 |     /* Find the current thread's index in thread_table for round-robin fairness */
 201 |     int start_idx = 0;
 202 |     for (int i = 0; i < MAX_THREADS; i++) {
 203 |         if (thread_table[i] == prev) {
 204 |             start_idx = (i + 1) % MAX_THREADS;
 205 |             break;
 206 |         }
 207 |     }
 208 | 
       //// Scan circularly across all 64 slots for the first candidate satisfying:
       //// 1. cand->state == THREAD_READY (note: a thread currently running on ANOTHER
       //// CPU has state == THREAD_RUNNING, so two CPUs can NEVER simultaneously pick the
       //// same thread!).
       //// 2. CPU affinity match: cand->pinned_cpu == -1 (unpinned) OR cand->pinned_cpu
       //// == cpu.
 209 |     for (int step = 0; step < MAX_THREADS; step++) {
 210 |         int idx = (start_idx + step) % MAX_THREADS;
 211 |         thread_t *cand = thread_table[idx];
 212 |         if (!cand) continue;
 213 |         if (cand->state == THREAD_READY &&
 214 |             (cand->pinned_cpu == -1 || cand->pinned_cpu == (int)cpu)) {
 215 |             next = cand;
 216 |             break;
 217 |         }
 218 |     }
 219 | 
       //// Handle the case where NO other THREAD_READY candidate exists for this CPU:
       //// - If 'prev' was merely yielding/preempting (prev->state == THREAD_RUNNING), keep running 'prev' with zero context-switch overhead!
       //// - If 'prev' is blocking, sleeping, or exiting (prev->state != THREAD_RUNNING), fall back to this core's dedicated &idle_threads[cpu].
 220 |     if (!next) {
 221 |         if (prev->state == THREAD_RUNNING) {
 222 |             return; /* Keep running prev */
 223 |         }
 224 |         next = &idle_threads[cpu];
 225 |     }
 226 | 
 227 |     if (next == prev) {
 228 |         next->state = THREAD_RUNNING;
 229 |         return;
 230 |     }
       //// Commit the context switch on 'cpu':
       //// 1. Demote prev from THREAD_RUNNING to THREAD_READY (ONLY if it was still
       //// THREAD_RUNNING — preserving BLOCKED, SLEEPING, or DEATH states!).
       //// 2. Promote next to THREAD_RUNNING and record current_threads[cpu] = next.
       //// 3. Lazy MMU Address-Space Switch: Only rewrite TTBR0_EL1 via
       //// vmm_switch_aspace(next->aspace) when prev->aspace != next->aspace (avoiding
       //// unnecessary 'msr ttbr0_el1' + 'isb' pipeline flushes when switching between
       //// two threads of the same process or two kernel/idle threads!).
       //// 4. Invoke arm64_context_switch(&prev->kernel_sp, next->kernel_sp). When 'prev'
       //// is eventually scheduled again (possibly on a DIFFERENT CPU if unpinned!),
       //// arm64_context_switch returns right here at line 243.
 231 | 
 232 |     if (prev->state == THREAD_RUNNING) {
 233 |         prev->state = THREAD_READY;
 234 |     }
 235 |     next->state = THREAD_RUNNING;
 236 |     current_threads[cpu] = next;
 237 | 
 238 |     if (prev->aspace != next->aspace) {
 239 |         vmm_switch_aspace(next->aspace);
 240 |     }
 241 | 
 242 |     arm64_context_switch(&prev->kernel_sp, next->kernel_sp);
 243 | }
 244 | 
       //// thread_yield() & thread_preempt():
       //// - thread_yield(): Voluntary reschedule point acquiring sched_lock with IRQs saved and calling sched_reschedule_locked().
       //// - thread_preempt(): Involuntary preemption hook called at line 85 of exceptions_c.c at the end of every hardware IRQ. If thread_timer_tick() set need_resched[cpu] = 1, clears the flag and yields the CPU.
 245 | void thread_yield(void) {
 246 |     uint64_t irqstate;
 247 |     spin_lock_irqsave(&sched_lock, &irqstate);
 248 |     sched_reschedule_locked();
 249 |     spin_lock_irqrestore(&sched_lock, irqstate);
 250 | }
 251 | 
 252 | void thread_preempt(void) {
 253 |     uint32_t cpu = arch_curr_cpu_num();
 254 |     if (need_resched[cpu]) {
 255 |         need_resched[cpu] = 0;
 256 |         thread_yield();
 257 |     }
 258 | }
 259 | 
       //// thread_sleep_ns() & thread_exit():
       //// - thread_sleep_ns(): Computes absolute monotonic wakeup deadline (current_time_ns() + delay_ns), transitions cur->state = THREAD_SLEEPING, and yields the CPU under sched_lock. Powers sys_nanosleep().
       //// - thread_exit(): Records retcode, marks cur->state = THREAD_DEATH, and calls sched_reschedule_locked(), which never returns to a dead thread; the trailing 'wfi' loop satisfies compiler noreturn analysis.
 260 | void thread_sleep_ns(ab_time_t delay_ns) {
 261 |     uint64_t irqstate;
 262 |     spin_lock_irqsave(&sched_lock, &irqstate);
 263 |     thread_t *cur = get_current_thread();
 264 |     cur->wake_time_ns = current_time_ns() + delay_ns;
 265 |     cur->state = THREAD_SLEEPING;
 266 |     sched_reschedule_locked();
 267 |     spin_lock_irqrestore(&sched_lock, irqstate);
 268 | }
 269 | 
 270 | void thread_exit(int retcode) {
 271 |     uint64_t irqstate;
 272 |     spin_lock_irqsave(&sched_lock, &irqstate);
 273 |     thread_t *cur = get_current_thread();
 274 |     cur->retcode = retcode;
 275 |     cur->state   = THREAD_DEATH;
 276 |     sched_reschedule_locked();
 277 |     spin_lock_irqrestore(&sched_lock, irqstate);
 278 |     for (;;) __asm__ volatile("wfi");
 279 | }
 280 | 
       //// thread_timer_tick(): Called at 100 Hz on EVERY CPU from timer_irq_handler().
       //// Acquires sched_lock and performs two vital time-driven duties:
       //// 1. Wakes any THREAD_SLEEPING thread whose wake_time_ns <= now (setting state =
       //// THREAD_READY + 'sev').
       //// 2. Wakes any timed THREAD_BLOCKED wait-queue thread whose non-zero deadline
       //// wake_time_ns <= now: removes it from t->blocking_wq while holding sched_lock,
       //// clears wake_time_ns, sets t->wait_status = AB_ERR_TIMED_OUT, and marks it
       //// THREAD_READY!
       //// Finally, sets need_resched[cpu] = 1 so the current CPU preempts its running
       //// thread upon exiting the timer IRQ.
 281 | void thread_timer_tick(void) {
 282 |     uint32_t cpu = arch_curr_cpu_num();
 283 |     ab_time_t now = current_time_ns();
 284 | 
 285 |     uint64_t irqstate;
 286 |     spin_lock_irqsave(&sched_lock, &irqstate);
 287 | 
 288 |     for (int i = 0; i < MAX_THREADS; i++) {
 289 |         thread_t *t = thread_table[i];
 290 |         if (!t) continue;
 291 |         if (t->state == THREAD_SLEEPING && now >= t->wake_time_ns) {
 292 |             t->state = THREAD_READY;
 293 |             __asm__ volatile("sev");
 294 |         } else if (t->state == THREAD_BLOCKED && t->wake_time_ns != 0 && now >= t->wake_time_ns) {
 295 |             /* Timed wait_queue wakeup (used in Lesson 06+) */
 296 |             if (t->blocking_wq) {
 297 |                 wait_queue_remove_thread_locked(t->blocking_wq, t);
 298 |             }
 299 |             t->wake_time_ns = 0;
 300 |             t->wait_status  = AB_ERR_TIMED_OUT;
 301 |             t->state        = THREAD_READY;
 302 |             __asm__ volatile("sev");
 303 |         }
 304 |     }
 305 | 
 306 |     need_resched[cpu] = 1;
 307 |     spin_lock_irqrestore(&sched_lock, irqstate);
 308 | }
 309 | 
       //// thread_become_idle(): Terminal execution loop of all four CPUs' idle threads
       //// (CPUs 1..3 enter from arm64_secondary_entry(); CPU 0 enters at the end of
       //// lk_main()).
       //// Unmasks local IRQs, reaps any dead detached threads, yields to any runnable
       //// work, and executes 'wfi' (Wait For Interrupt) to halt the Cortex-A53 clock
       //// until the next GIC interrupt or 'sev' wakeup arrives.
 310 | void thread_become_idle(void) {
 311 |     arch_enable_ints();
 312 |     for (;;) {
 313 |         thread_reap_dead();
 314 |         thread_yield();
 315 |         __asm__ volatile("wfi");
 316 |     }
 317 | }
```
### 8.8.3 Technical Notes

1. **Tracing the Cross-Thread `sched_lock` Handoff Across Two Different CPUs**:
   To appreciate why `sched_lock` is subtle in an SMP kernel, trace what happens when Thread A (running on CPU 0) blocks in `event_wait_deadline()`, and later CPU 2 wakes Thread A via `event_signal()`:
   - On **CPU 0**, Thread A calls `sched_lock_acquire(&irqstate_A)` (saving `DAIF_A` on Thread A's stack and locking `sched_lock`) -> `sched_reschedule_locked()` -> `arm64_context_switch(&A->kernel_sp, B->kernel_sp)`.
   - Thread B resumes on **CPU 0** at line 243 of `thread.c`, returns from `sched_reschedule_locked()` to *Thread B's* caller (say, `thread_yield()`), and calls `spin_lock_irqrestore(&sched_lock, irqstate_B)` — unlocking `sched_lock` and restoring *Thread B's* saved `DAIF_B`!
   - Later, **CPU 2** marks Thread A `THREAD_READY`. Suppose **CPU 1** is idle and picks up Thread A inside `thread_yield()`: CPU 1 acquires `sched_lock` (saving `irqstate_idle1`) and switches to Thread A!
   - Thread A now wakes up at line 243 **on CPU 1** (having gone to sleep on CPU 0!), unwinds to `event_wait_deadline()`, and calls `sched_lock_release(irqstate_A)` — unlocking the `sched_lock` that CPU 1 acquired, and restoring Thread A's original `DAIF_A`! Because `DAIF` only encodes interrupt mask bits (which are CPU-agnostic), migrating across CPUs mid-switch works seamlessly!

2. **Why `idle_threads[cpu]` Never Contends on `thread_table[]`**:
   Keeping `idle_threads[0..3]` in a dedicated per-CPU array outside `thread_table[]` ensures two crucial properties: (a) an idle thread can never be accidentally stolen by another CPU, and (b) all 64 slots of `thread_table[]` remain available for real kernel and user threads.

---

## 8.9 `kernel/core/wait.c`

### 8.9.1 Functional Description

Busy-waiting wastes CPU cycles and deadlocks single-core priority chains; every microkernel IPC primitive needs a way to put threads to sleep with zero CPU consumption until either a capability signal arrives or a nanosecond deadline expires. `kernel/core/wait.c` provides two layered building blocks:

1. **`wait_queue_t`**: An intrusive FIFO queue (`head`, `tail`, `count`) threading directly through `thread_t.wait_next`. All `_locked` operations execute under `sched_lock`, guaranteeing atomicity with `thread_timer_tick()` timeouts.
2. **`event_t`**: Wraps a `wait_queue_t` with a boolean `signaled` latch and `flags` (`0` for sticky broadcast wakeup, or `EVENT_FLAG_AUTOUNSIGNAL` for single-waiter semaphore consumption).

| Consumer in ABOS (`object.c`) | Primitive & Mode | Why That Mode Is Chosen |
| :--- | :--- | :--- |
| `ab_wait_observer_t.event` (`sys_object_wait_one`) | `event_t` (`flags = 0`, sticky) | Observer lives on the stack of one waiting thread; sticky signal avoids lost wakeups |
| `ab_port_dispatcher_t.readable_event` (`sys_port_wait`) | `event_t` (`EVENT_FLAG_AUTOUNSIGNAL`) | Wakes one worker per queued packet; re-signaled if `port->count > 0` remains |
| `ab_interrupt_dispatcher_t.irq_event` (`sys_interrupt_wait`) | `event_t` (`EVENT_FLAG_AUTOUNSIGNAL`) | Consumes exactly one hardware IRQ trigger per `sys_interrupt_wait()` call |

### 8.9.2 Heavily Commented Source Code
#### Program 8-8: `lesson_20/kernel/core/wait.c`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/wait.c" start="1" end="151" total="151" -->
```c
       //// kernel/core/wait.c implements ABOS's two foundational blocking synchronization
       //// primitives — wait_queue_t and event_t — modeled directly after Zircon's
       //// WaitQueue and Event (zircon/kernel/kernel/{wait,event}.cpp). Every
       //// higher-level blocking operation in the microkernel (sys_object_wait_one via
       //// StateTracker, sys_port_wait on PortDispatcher, and sys_interrupt_wait on
       //// InterruptDispatcher) ultimately sleeps and wakes through these 151 lines.
   1 | /* =========================================================================
   2 |  * wait.c - LK Wait Queues (wait_queue_t) & Events (event_t) Implementation
   3 |  * References:
   4 |  *   - zircon_org/kernel/kernel/wait.cpp
   5 |  *   - zircon_org/kernel/kernel/event.cpp
   6 |  * ========================================================================= */
   7 | #include "wait.h"
   8 | #include "spinlock.h"
   9 | #include "debuglog.h"
  10 | 
       //// wait_queue_init(): Initializes an empty O(1) FIFO queue (head = NULL, tail =
       //// NULL, count = 0). Because wait_queue_t threads nodes intrusively through
       //// thread_t.wait_next, blocking a thread NEVER allocates heap memory and can
       //// never fail with ENOMEM!
  11 | void wait_queue_init(wait_queue_t *wq) {
  12 |     wq->head  = NULL;
  13 |     wq->tail  = NULL;
  14 |     wq->count = 0;
  15 | }
  16 | 
       //// wait_queue_remove_thread_locked(): Unlinks a specific 'target' thread from
       //// 'wq' (caller MUST hold sched_lock!).
       //// Used in two scenarios:
       //// 1. Timer timeout inside thread_timer_tick() (thread.c line 297) when a thread
       //// blocked with a finite deadline expires before the event is signaled.
       //// 2. Defensive post-wakeup cleanup at line 66 of wait_queue_block_locked().
       //// Maintains head, tail, count, wait_next, and blocking_wq invariants across
       //// head, middle, and tail removals.
  17 | void wait_queue_remove_thread_locked(wait_queue_t *wq, thread_t *target) {
  18 |     if (!wq || !target) return;
  19 |     thread_t *prev = NULL;
  20 |     thread_t *curr = wq->head;
  21 |     while (curr) {
  22 |         if (curr == target) {
  23 |             if (prev) {
  24 |                 prev->wait_next = curr->wait_next;
  25 |             } else {
  26 |                 wq->head = curr->wait_next;
  27 |             }
  28 |             if (wq->tail == curr) {
  29 |                 wq->tail = prev;
  30 |             }
  31 |             curr->wait_next   = NULL;
  32 |             curr->blocking_wq = NULL;
  33 |             if (wq->count > 0) wq->count--;
  34 |             return;
  35 |         }
  36 |         prev = curr;
  37 |         curr = curr->wait_next;
  38 |     }
  39 |     target->blocking_wq = NULL;
  40 | }
  41 | 
       //// wait_queue_block_locked(wait_queue_t *wq, ab_time_t deadline_ns): Atomically
       //// places the calling thread to sleep on 'wq' (caller MUST hold sched_lock with
       //// IRQs disabled!).
       //// - deadline_ns == 0 (AB_TIME_INFINITE) means wait forever; otherwise lines 45-47 check whether the deadline has ALREADY passed before sleeping, returning AB_ERR_TIMED_OUT immediately.
       //// - Initializes wait_status = AB_OK, records deadline_ns and blocking_wq, sets cur->state = THREAD_BLOCKED, and appends 'cur' to the tail of 'wq' in O(1) time for strict FIFO wakeup order.
  42 | int wait_queue_block_locked(wait_queue_t *wq, ab_time_t deadline_ns) {
  43 |     thread_t *cur = get_current_thread();
  44 | 
  45 |     if (deadline_ns != 0 && current_time_ns() >= deadline_ns) {
  46 |         return AB_ERR_TIMED_OUT;
  47 |     }
  48 | 
  49 |     cur->wait_status  = AB_OK;
  50 |     cur->wake_time_ns = deadline_ns;
  51 |     cur->blocking_wq  = wq;
  52 |     cur->wait_next    = NULL;
  53 |     cur->state        = THREAD_BLOCKED;
  54 | 
  55 |     if (wq->tail) {
  56 |         wq->tail->wait_next = cur;
  57 |         wq->tail = cur;
  58 |     } else {
  59 |         wq->head = wq->tail = cur;
  60 |     }
  61 |     wq->count++;
  62 | 
       //// Context-switch away while holding sched_lock!
       //// sched_reschedule_locked() suspends 'cur' and switches this CPU to another
       //// thread. 'cur' stays frozen at line 63 until EITHER:
       //// - A signaler calls wait_queue_wake_one/all_locked(wq, status), setting cur->wait_status = status (e.g., AB_OK or AB_ERR_CANCELED); OR
       //// - thread_timer_tick() observes now >= cur->wake_time_ns, unlinks 'cur', and sets cur->wait_status = AB_ERR_TIMED_OUT.
       //// When 'cur' resumes here (still holding sched_lock!), line 66 ensures 'cur' is
       //// unlinked from 'wq' in all cases and returns cur->wait_status.
  63 |     sched_reschedule_locked();
  64 | 
  65 |     /* If woken by timer timeout, ensure we are removed from wq */
  66 |     wait_queue_remove_thread_locked(wq, cur);
  67 |     return cur->wait_status;
  68 | }
  69 | 
       //// wait_queue_wake_one_locked(): Pops the oldest waiter from wq->head in O(1)
       //// time (FIFO order), clears its wait-queue links and timeout deadline, writes
       //// wait_status into t->wait_status, transitions t->state = THREAD_READY, executes
       //// 'sev' so an idle SMP core wakes immediately to run 't', and returns 1 (or 0 if
       //// the queue was empty).
  70 | int wait_queue_wake_one_locked(wait_queue_t *wq, int wait_status) {
  71 |     while (wq->head) {
  72 |         thread_t *t = wq->head;
  73 |         wq->head = t->wait_next;
  74 |         if (!wq->head) wq->tail = NULL;
  75 |         t->wait_next   = NULL;
  76 |         t->blocking_wq = NULL;
  77 |         if (wq->count > 0) wq->count--;
  78 | 
  79 |         if (t->state == THREAD_BLOCKED) {
  80 |             t->wait_status  = wait_status;
  81 |             t->wake_time_ns = 0;
  82 |             t->state        = THREAD_READY;
  83 |             __asm__ volatile("sev");
  84 |             return 1;
  85 |         }
  86 |     }
  87 |     return 0;
  88 | }
  89 | 
       //// wait_queue_wake_all_locked(): Repeatedly invokes wait_queue_wake_one_locked()
       //// until the queue is empty, returning the total count of woken threads. Used by
       //// broadcast events and object teardown (which wakes waiters with AB_ERR_CANCELED
       //// / PEER_CLOSED).
  90 | int wait_queue_wake_all_locked(wait_queue_t *wq, int wait_status) {
  91 |     int woken = 0;
  92 |     while (wait_queue_wake_one_locked(wq, wait_status)) {
  93 |         woken++;
  94 |     }
  95 |     return woken;
  96 | }
  97 | 
       //// event_init(): Initializes a kernel event_t in either:
       //// - Manual-reset / sticky broadcast mode (flags == 0): Once signaled, e->signaled stays 1 and wakes ALL current and future waiters until event_unsignal() is called (used by ab_wait_observer_t in StateTracker).
       //// - Auto-unsignal / binary-semaphore mode (flags == EVENT_FLAG_AUTOUNSIGNAL): Each event_signal() releases at most ONE waiter and automatically consumes the signal (used by PortDispatcher's readable_event and InterruptDispatcher's irq_event!).
  98 | void event_init(event_t *e, int initial, uint32_t flags) {
  99 |     e->signaled = initial ? 1 : 0;
 100 |     e->flags    = flags;
 101 |     wait_queue_init(&e->wait);
 102 | }
 103 | 
       //// event_wait_deadline() & event_wait():
       //// Acquires sched_lock (disabling local IRQs) to check e->signaled and enqueue
       //// onto e->wait ATOMICALLY — completely eliminating the classic 'lost wakeup'
       //// race where an IRQ or remote CPU signals an event between checking a flag and
       //// going to sleep!
       //// - Fast path (e->signaled != 0): Returns AB_OK immediately without context-switching (clearing e->signaled = 0 if EVENT_FLAG_AUTOUNSIGNAL is set).
       //// - Slow path (e->signaled == 0): Sleeps via wait_queue_block_locked(&e->wait, deadline_ns).
 104 | int event_wait_deadline(event_t *e, ab_time_t deadline_ns) {
 105 |     uint64_t irqstate;
 106 |     sched_lock_acquire(&irqstate);
 107 | 
 108 |     int ret = AB_OK;
 109 |     if (e->signaled) {
 110 |         if (e->flags & EVENT_FLAG_AUTOUNSIGNAL) {
 111 |             e->signaled = 0;
 112 |         }
 113 |     } else {
 114 |         ret = wait_queue_block_locked(&e->wait, deadline_ns);
 115 |     }
 116 | 
 117 |     sched_lock_release(irqstate);
 118 |     return ret;
 119 | }
 120 | 
 121 | int event_wait(event_t *e) {
 122 |     return event_wait_deadline(e, 0);
 123 | }
 124 | 
       //// event_signal(): Signals event 'e' safely from either thread context OR
       //// hardware IRQ context (such as ab_interrupt_dispatch_hw_irq() when UART IRQ 33
       //// fires!):
       //// Look closely at the subtle difference between the two modes when !e->signaled:
       //// - EVENT_FLAG_AUTOUNSIGNAL (lines 131-135): Calls wait_queue_wake_one_locked() FIRST. If a thread was already waiting (woken == 1), that single thread consumes the signal directly so e->signaled stays 0! Only if NO thread was waiting (woken == 0) does e->signaled become 1 so the next future caller of event_wait() consumes it without blocking.
       //// - Standard broadcast mode (lines 137-138): Sets e->signaled = 1 sticky and wakes ALL currently blocked threads via wait_queue_wake_all_locked().
 125 | int event_signal(event_t *e) {
 126 |     uint64_t irqstate;
 127 |     sched_lock_acquire(&irqstate);
 128 | 
 129 |     int woken = 0;
 130 |     if (!e->signaled) {
 131 |         if (e->flags & EVENT_FLAG_AUTOUNSIGNAL) {
 132 |             woken = wait_queue_wake_one_locked(&e->wait, AB_OK);
 133 |             if (woken == 0) {
 134 |                 e->signaled = 1;
 135 |             }
 136 |         } else {
 137 |             e->signaled = 1;
 138 |             woken = wait_queue_wake_all_locked(&e->wait, AB_OK);
 139 |         }
 140 |     }
 141 | 
 142 |     sched_lock_release(irqstate);
 143 |     return woken;
 144 | }
 145 | 
       //// event_unsignal(): Explicitly resets e->signaled = 0 under sched_lock.
 146 | void event_unsignal(event_t *e) {
 147 |     uint64_t irqstate;
 148 |     sched_lock_acquire(&irqstate);
 149 |     e->signaled = 0;
 150 |     sched_lock_release(irqstate);
 151 | }
```
### 8.9.3 Technical Notes

1. **Why Timeout Cleanup Runs in Both `thread_timer_tick()` and `wait_queue_block_locked()`**:
   Look at line 297 of `thread.c` (`wait_queue_remove_thread_locked(t->blocking_wq, t)`) and line 66 of `wait.c` (`wait_queue_remove_thread_locked(wq, cur)`). When a timer expires, `thread_timer_tick()` unlinks `t` immediately under `sched_lock` (setting `t->blocking_wq = NULL`) so a concurrent `event_signal()` arriving *before* `t` actually gets scheduled onto a CPU will NOT mistake `t` for an active waiter and waste an `EVENT_FLAG_AUTOUNSIGNAL` wakeup on a thread that already timed out! Line 66 in `wait.c` is an inexpensive idempotent safety belt (since `wq->head` is normally empty or short, and if already removed, `cur` is no longer in `wq`).

2. **Comparison with Zircon's `OwnedWaitQueue` and Priority Inheritance**:
   Production Zircon (`zircon/kernel/kernel/owned_wait_queue.cc`) extends `WaitQueue` with `OwnedWaitQueue` to propagate scheduler priority/deadline profiles from blocked client threads to the server thread owning a channel call or futex (preventing Mars Pathfinder-style priority inversion). Because ABOS uses uniform equal-priority 10 ms round-robin scheduling across all threads, every runnable server thread is guaranteed bounded CPU service within at most `(N_runnable / 4) × 10 ms`.

---

## 8.10 Summary

Chapter 8 traced the complete trap, interrupt, timekeeping, context-switch, and SMP scheduling machinery of ABOS:
- **`exceptions.S`** & **`exceptions_c.c`** catch all EL0/EL1 traps and IRQs in 288-byte `iframe_t` frames, dispatch preemptible system calls, service lazy VMO demand-paging faults transparently, isolate crashing EL0 processes, and route hardware interrupts through GIC EOI into preemptive rescheduling.
- **`gic.c`** & **`timer.c`** drive the GICv2 controller and per-CPU 100 Hz ARMv8 Generic Timer (`PPI 30`), supplying both user-space driver SPI routing and overflow-safe monotonic nanosecond timekeeping (`current_time_ns()`).
- **`switch.S`** & **`uspace_entry.S`** pivot kernel stacks in 112-byte `context_switch_frame_t` records and drop new user threads cleanly into `EL0t` with scrubbed registers.
- **`thread.c`** & **`wait.c`** multiplex up to 64 kernel and user threads fairly across 4 SMP cores under `sched_lock`, reap exited detached thread stacks safely across cores, and provide race-free timed `wait_queue_t` and `event_t` synchronization.

Every kernel stack allocated by `thread_create()`, every `thread_t` allocated by `kzalloc()`, and every lazy user page faulted in by `arm64_sync_exception()` relies on ABOS's three-tier memory management subsystem. Chapter 9 dissects `pmm.c`, `heap.c`, and `mmu.c` in complete line-by-line detail.
