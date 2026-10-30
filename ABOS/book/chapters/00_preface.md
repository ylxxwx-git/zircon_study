# Preface

Operating system kernels sit at the exact intersection of computer architecture and software engineering. Every time an application allocates memory, exchanges a message with a system service, renders characters to a terminal, or reads sectors from a storage device, an intricate choreography unfolds across hardware privilege boundaries, Memory Management Units (MMUs), interrupt controllers, caches, and kernel data structures. Yet for generations of systems programmers and students, bridging the chasm between textbook operating system theory and real-world kernel source code has remained stubbornly difficult—especially in the domain of **modern 64-bit Symmetric Multiprocessing (SMP) capability-based microkernels**.

More than two decades ago, Dr. Zhao Jiong published his landmark work, *"A Heavily Commented Linux Kernel Source Code"* (*Linux 内核完全注释*), which guided hundreds of thousands of engineers line by line through every assembly instruction, C function, header file, and Makefile of Linus Torvalds's classic `Linux 0.12` kernel (~19,000 lines on 32-bit Intel 80386). By refusing to omit "boring" glue code or hide hardware subtleties behind hand-waving abstractions, that book proved a timeless pedagogical truth: **once you have read one complete, self-contained, working operating system kernel from its first reset instruction to its interactive user-space shell, no larger operating system will ever feel opaque or mysterious again.**

This book brings that exact cover-to-cover, line-by-line annotation tradition into the modern era of **64-bit ARM (`AArch64` / `ARMv8-A`) multi-core processors** and **object-capability microkernel architecture**. Its subject is **ABOS (Lesson 20)**—a complete, compilable, bootable 4-core SMP capability microkernel operating system modeled directly after **Google Fuchsia's Zircon kernel**, written in clean freestanding C11 and GNU AArch64 assembly across **58 source and build files totaling exactly 9,526 lines**.

---

## The Main Goal of This Book

The primary goal of this book is to help readers achieve **complete, end-to-end mastery** of both **ARMv8-A 64-bit multi-core system programming** and **Zircon-style capability microkernel design** by dissecting every single file and line of ABOS Lesson 20.

Specifically, after working through this book, the reader will understand from first principles and concrete code:

1. **How a 64-bit ARM multi-core machine boots from bare metal to higher-half virtual memory**: Dropping from `EL2` to `EL1h`, building early 1 GB block page tables (`.idmap`), enabling the `MMU` alongside instruction and data caches (`SCTLR_EL1`), waking secondary Cortex-A53 cores via `PSCI_CPU_ON` (`HVC #0`), and parsing the hardware Flattened Device Tree (`DTB`) and `BOOTFS` initial RAM disk.
2. **How a two-layer Zircon-inspired microkernel is structured internally**: Separating the low-level **LK (Little Kernel) hardware and scheduling substrate** (physical frame arenas, kernel heap, 3-level `TTBR0_EL1` user page tables with hardware ASIDs, preemptive SMP round-robin scheduling, and blocking `wait_queue_t` / `event_t` primitives) from the high-level **Zircon Object & Capability layer** (`ab_object_t` dispatchers, generation-tagged capability handles, bitmask rights, `StateTracker` signal observers, asynchronous `Port` packet queues, `Channel` message-and-handle transfer, and `VMO` / `VMAR` demand-paged shared memory).
3. **How user-space bootstrapping, `EL0` device drivers, filesystems, virtual terminals, and shells collaborate without in-kernel drivers**: Watching `lk_main` hand root capabilities over a `Channel` to `userboot`, which unpacks `bootfs.tar` in `EL0` and launches standalone user-space servers—`console-srv.bin` (driving the PL011 UART on SPI IRQ 33 and multiplexing 4 virtual terminals `tty0..tty3` on a `Port`), `fs-srv.bin` (driving VirtIO-Blk DMA rings in `EL0` via `Resource`, contiguous DMA `VMO`, and `Interrupt` capabilities), four isolated `abos-shell` instances, and on-demand disk applications such as `calc.bin`.

Rather than stopping at architectural block diagrams, we trace every bit of every hardware register (`TCR_EL1`, `MAIR_EL1`, `ESR_EL1`, `FAR_EL1`, `GICD`/`GICC`, `CNTP_TVAL_EL0`), every spinlock acquisition order, every cache and TLB maintenance barrier (`tlbi vae1is`, `dc cvau`, `ic ialluis`), and every capability handle lifecycle transition (`ref_count` vs. `handle_count`).

---

## Features of This Book

Existing operating system literature generally falls into two extremes:

- **Conceptual OS textbooks** explain scheduling policies, page replacement algorithms, and access-control matrices in abstract pseudocode, leaving readers unsure how `SP_EL0` switches to `SP_EL1` on an `SVC #0` trap, how a level-triggered device IRQ is masked before waking an `EL0` driver thread, or how a linker script separates virtual execution addresses (`VMA`) from physical load addresses (`LMA`).
- **Production open-source kernels** have grown far beyond what an individual engineer can read end-to-end. Modern Linux exceeds **30 million lines of code** across dozens of architectures and legacy subsystems; even Google Fuchsia's production **Zircon** kernel spans **over 350,000 lines of heavily templated C++** (`fbl::RefPtr`, `ktl`, lock-dep template metaprogramming, FIDL/vDSO code generators, and multi-board platform trees).

This book bridges the gap between those two extremes with four defining features:

1. **100% Complete Source Code Coverage (Zero Omissions)**: Part II of this book reproduces and annotates all **58 files (9,526 lines)** of ABOS Lesson 20—including every kernel `.S` and `.c` file, every header in `include/`, both linker scripts (`linker.ld` and `user/link.ld`), every `EL0` C runtime file (`crt0.S`, `crt_env.c`, `ulibc.c`), `userboot`, both `EL0` device driver servers (`console_srv.c`, `fs_srv.c`), the interactive shell (`shell.c`), `calc.c`, the Python image packagers, and the hierarchical Makefiles.
2. **Faithful Zircon Architecture in Transparent Freestanding C**: ABOS translates Zircon's C++ `Dispatcher` class hierarchy into crystal-clear C structures (`ab_object_t` base header, explicit `on_zero_handles` and `destroy` hooks, explicit `ref_count` and `handle_count`, and explicit `spin_lock_irqsave` critical sections). There is zero hidden C++ constructor/destructor magic, zero operator overloading, and zero macro obfuscation.
3. **Structured ECLK Pedagogical Format**: Modeled directly on Dr. Zhao Jiong's classic structure, Part I (Chapters 1–5) builds a solid foundation in history, QEMU `virt` hardware, the GNU AArch64 toolchain, ARMv8-A system architecture, and ABOS's global design. Part II (Chapters 6–16) dissects each subsystem module with functional overviews, line-numbered verbatim listings interleaved with author commentary, and post-code deep dives. Part III (Chapter 17 and Appendices) provides a hands-on QEMU + `gdb-multiarch` source-level debugging laboratory and complete quick-reference tables.
4. **Immediate Hands-On Reproducibility**: With a single command (`make run`), ABOS Lesson 20 compiles from scratch in under two seconds on any Linux or macOS host and boots a 4-core Cortex-A53 virtual machine in QEMU with four live virtual terminals (`tty0..tty3`), real DMA block I/O, and live Zircon-style task/VM introspection (`ps`, `vmaps`, `caps`).

---

## Monolithic Kernels vs. Capability Microkernels

For half a century, operating system architecture has been shaped by a fundamental design tension: **what belongs inside the privileged kernel mode (`EL1`), and how is authority granted to software?**

In a **classic monolithic kernel**—exemplified by UNIX, BSD, Linux, and ABOS's companion sister project **AAOS**—the privileged `EL1` kernel binary incorporates process scheduling, virtual memory, filesystems, block storage drivers, TTY line disciplines, and networking stacks into a single shared kernel address space. User processes invoke ambient-authority POSIX system calls (`open("/dev/sda", ...)`, `read(fd, ...)`, `write(fd, ...)`), and access control is mediated by global namespaces (file paths, UIDs/GIDs). Monolithic kernels enjoy low-latency in-kernel function calls, but they pay a steep price in **Trusted Computing Base (TCB) size** and **fault containment**: a single out-of-bounds pointer write or use-after-free bug inside a third-party device driver or filesystem parser executes with full `EL1` privilege and compromises the entire machine.

A **capability-based microkernel**—pioneered conceptually by Dennis and Van Horn (1966), refined for high-performance IPC by Jochen Liedtke's **L4** family and **seL4**, and brought to modern consumer and embedded scale by **Google Fuchsia's Zircon**—radically inverts both choices:

| Architectural Dimension | Monolithic Kernel (UNIX / Linux / AAOS) | Capability Microkernel (Fuchsia Zircon / ABOS) |
| :--- | :--- | :--- |
| **Privileged `EL1` TCB Scope** | Scheduler, VM, VFS, filesystems, block/TTY/net drivers all run in `EL1`. | **Minimal `EL1` core**: Only scheduling, VM (`VMO`/`VMAR`), IPC (`Channel`/`Port`/`Event`), and hardware brokering (`Resource`/`Interrupt`). |
| **Device Drivers & Filesystems** | Compiled into `EL1` or loaded as privileged kernel modules (`.ko`). | Run as **unprivileged `EL0` user-space server processes** (`console-srv.bin`, `fs-srv.bin`) isolated in private `TTBR0_EL1` page tables. |
| **Authority & Naming Model** | **Ambient authority**: Any process can attempt to open global paths or device nodes based on UID/GID. | **Object-capability model**: Zero global object namespace. A process can act **only** on unforgeable `ab_handle_t` capabilities explicitly transferred into its private handle table. |
| **Permission Granularity** | Coarse file mode bits (`rwx`) or broad `CAP_SYS_ADMIN` flags. | Fine-grained per-handle **Rights bitmask** (`AB_RIGHT_READ`, `WRITE`, `MAP`, `EXECUTE`, `DUPLICATE`, `TRANSFER`, `MANAGE_PROCESS`) monotonically attenuated on duplication. |
| **Inter-Component Communication** | Direct C function calls across subsystems in shared `EL1` memory. | Small control messages + capability handle transfer over **`Channel`s**, paired with **zero-copy shared `VMO`s** and **`Port`** event loops. |
| **Fault Isolation** | A null-pointer dereference in a UART or block driver triggers an `EL1` kernel panic. | An unhandled fault in an `EL0` process triggers `ab_process_exit_current(-1)`, terminating only that process while `EL1` and sibling processes keep running. |

Studying ABOS allows you to see that a capability microkernel does not merely "move code to user space"—it replaces global state with explicit **capability graphs**. For instance, `abos-shell` in Lesson 20 holds five handles (`DebugLog`, its own `VMAR`, a `console-srv` `Channel`, an `fs-srv` `Channel`, and a `Job` handle), and **zero** `Resource` or `Interrupt` capabilities; it is architecturally impossible for the shell or `calc.bin` to touch physical MMIO addresses or claim hardware IRQs, even without a single UID check in the kernel.

---

## The Importance and Necessity of Reading the Complete Code

In the systems community, the maxim **RTFSC** (*"Read The F\*cking Source Code"*) exists for a profound reason: prose descriptions necessarily compress away edge cases, invariants, and cross-module interactions, whereas source code cannot lie. Yet *how* one reads kernel source code matters just as much as *whether* one reads it.

Many learners attempt to study a large operating system by reading isolated snippets—say, the 40 lines of `context_switch` or a single `page_fault` handler—in isolation. That approach inevitably leads to the classic **"blind men and the elephant"** dilemma:

- Without reading `linker.ld` and `start.S`, you cannot see why subtracting `KERNEL_PHYSMAP_OFFSET` (`0xFFFFFFFF40000000`) converts a kernel stack pointer back into a physical page frame address in `thread_reap_dead()`.
- Without reading `exceptions.S` alongside `uspace_entry.S` and `thread.c`, you cannot see how a newly spawned thread starts on its `context_switch_frame_t` via `ret` into `initial_thread_trampoline()`, drops to `EL0` via `eret` in `arm64_uspace_entry()`, and later traps back into `EL1` via `SVC #0` pushing a 288-byte `iframe_t` onto the *exact same* 8 KB kernel stack.
- Without reading `object.c` alongside `console_srv.c`, `fs_srv.c`, and `shell.c`, you cannot appreciate why `ab_object_t` maintains **two separate counters** (`ref_count` for kernel memory lifetime and `handle_count` for user-space handle lifetime), or how closing the last handle on one end of a `Channel` asserts `AB_SIGNAL_PEER_CLOSED` on the peer endpoint and wakes a `Port` loop in `fs-srv`.
- Without reading `image/Makefile`, `user/link.ld`, and `userboot.c`, the boundary between compile-time linking (`0x20000000`), bootloader `initrd` handoff (`bootfs.tar`), and runtime `VMO`/`VMAR` mapping remains magical rather than mechanical.

Reading a **complete** operating system from top to bottom dissolves all of those boundaries at once. Every symbol defined in one file is consumed in another file a few chapters away; nothing is swept behind an unexamined black box.

---

## Why Choose ABOS Lesson 20 (~9,526 Lines) to Read?

Choosing the right codebase for a complete source-code walkthrough requires a delicate balance: if the kernel is too toy-like (e.g., a 1,000-line cooperative single-core demo), it omits the very mechanisms—SMP spinlocks, ASID-tagged multi-level page tables, lazy demand paging, capability rights, asynchronous `Port` multiplexing, DMA block drivers, and standalone user-space servers—that define real operating systems. Conversely, if the kernel is hundreds of thousands or millions of lines long, no human reader can hold the whole system in working memory.

Table 0-1 compares ABOS Lesson 20 against classic educational and production kernels:

| Operating System / Version | Target Architecture | Kernel Paradigm | Language | Approx. Source Scale | Suitability for Complete Line-by-Line Study |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **UNIX V6** (Lions' Commentary, 1976) | 16-bit PDP-11 | Monolithic | Pre-ANSI C & PDP-11 ASM | ~9,000 lines | Historic masterpiece, but targets obsolete 16-bit single-core hardware without modern MMUs or capabilities. |
| **Linux 0.12** (Zhao Jiong ECLK, 1992) | 32-bit x86 (i386) | Monolithic | C89 & x86 GAS/as86 | ~19,500 lines | Gold standard for classic x86 monolithic UNIX internals, but single-core 32-bit segmented x86. |
| **xv6-riscv** (MIT 6.1810) | 64-bit RISC-V | Monolithic | Freestanding C & RV64 ASM | ~9,000 lines | Excellent compact SMP monolithic UNIX teaching OS, but not ARMv8-A and not a capability microkernel. |
| **Fuchsia Zircon** (Production) | AArch64 / x86-64 / RISC-V | Capability Microkernel | Modern C++20 & ASM | ~350,000+ lines | Industrial reference for capability microkernels, but far too large and C++-heavy for a single-volume complete code walkthrough. |
| **Linux 6.x** (Production) | 20+ Architectures | Monolithic (+ Modules) | GNU C11 & ASM | >30,000,000 lines | Impossible to read completely; studied only one subsystem slice at a time. |
| **ABOS Lesson 20** (*This Book*) | **64-bit ARMv8-A (4-Core SMP)** | **Zircon-Style Capability Microkernel** | **Freestanding C11 & AArch64 GAS** | **9,526 lines (58 files)** | **Sweet spot**: Complete 4-core SMP `EL1` microkernel + `userboot` + `EL0` UART/VT server + `EL0` VirtIO-Blk/TAR server + 4 shells + `ps`/`vmaps` + `calc` app, readable cover-to-cover in a single book! |

At **9,526 lines across 58 files** (~5,826 lines of `EL1` kernel, headers, and linker/build scripts, plus ~2,338 lines of `EL0` runtime, servers, shells, and apps, and ~1,362 lines of shared interface headers and image tooling), ABOS Lesson 20 is half the size of `Linux 0.12` while implementing a genuinely modern 64-bit multi-core capability microkernel ecosystem. One dedicated month of evening reading—with QEMU and GDB open alongside the text—is sufficient to master every line in the repository.

---

## Basic Knowledge Required by This Book

To gain the maximum benefit from this book, readers should be comfortable with the following foundational topics (Chapters 2, 3, and 4 also review every ABOS-specific detail before Part II begins):

1. **C Programming at the Systems Level**: Pointers, pointer arithmetic, fixed-width integer types (`uint64_t`, `uintptr_t`), bitwise shifts and masks, structs, function pointers, `volatile` memory accesses, and basic data structures (singly/doubly linked lists and ring buffers).
2. **Computer Organization & Assembly Concepts**: CPU registers, program counter (`PC`), stack pointer (`SP`), stack frames, hardware interrupts vs. synchronous traps, physical vs. virtual addresses, and caches/TLBs. Prior familiarity with any assembly language (x86, RISC-V, MIPS, or ARM) is helpful; Chapters 3 and 4 introduce every AArch64 instruction and system register used in ABOS.
3. **Command-Line Development on Linux or macOS**: Running `make`, inspecting binaries with `objdump`/`readelf`/`nm`, and basic familiarity with terminal sessions.

### Recommended Companion References

Readers who wish to consult primary hardware specifications or explore the broader lineage behind ABOS will find the following works invaluable:

- **Zhao Jiong (赵炯)**, *A Heavily Commented Linux Kernel Source Code (Linux 内核完全注释, V5.0)* — The pedagogical inspiration for the structure and annotation style of this book.
- **ARM Limited**, *Arm Architecture Reference Manual for A-profile Architecture (ARM DDI 0487)* and *ARM Cortex-A Series Programmer's Guide for ARMv8-A* — Authoritative references for `EL0..EL3`, `VBAR_EL1`, Stage-1 VMSAv8-64 page tables, and AArch64 instructions.
- **Google Fuchsia Project**, *Zircon Kernel Concepts & System Call Documentation* (`docs/concepts.md`, `docs/handles.md`, `docs/rights.md`, `docs/signals.md`, `docs/userboot.md`) — Architectural documentation of the production Zircon kernel after which ABOS is modeled.
- **Jochen Liedtke**, *"On μ-Kernel Construction"* (SOSP 1995) and **Dennis & Van Horn**, *"Programming Semantics for Multiprogrammed Computations"* (CACM 1966) — Seminal papers on second-generation microkernels and capability-based protection.

---

## Sign-off

Writing an operating system from a blank assembly file up to a self-hosting multi-server microkernel—and then documenting every line of it—is an immensely rewarding journey. My hope is that ABOS Lesson 20 serves as a clear, welcoming glass box: small enough to hold completely in your head, yet faithful enough to modern industrial systems that when you subsequently open the Fuchsia Zircon, seL4, or Linux ARM64 source trees, you immediately recognize old friends on every page.

Every effort has been made to verify all line numbers, register bitfields, memory addresses, and behavioral descriptions directly against the runnable ABOS Lesson 20 source tree. Feedback, questions, and bug reports are always warmly welcomed.

<div align="right">

**Liping Liu**  
lipingliu@google.com  
October 2026

</div>
