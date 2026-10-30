# 3 Kernel Programming Language, Toolchain and Build Environment

An operating system kernel cannot rely on a host operating system's C standard library, dynamic linker, stack-setup runtime, or `POSIX` system calls—because the kernel *is* the lowest-level software responsible for creating address spaces and stacks in the first place. Even small compiler or linker subtleties—such as the compiler silently emitting a 128-bit `SIMD` register spill inside an `EL1` interrupt handler, reordering an MMIO register write across a spinlock boundary, or placing a boot trampoline at a virtual address when the `MMU` is still turned off—will crash a bare-metal multi-core machine instantaneously.

In *"A Heavily Commented Linux Kernel Source Code"*, Chapter 3 introduced `as86`, GNU `gas` AT&T x86 syntax, GCC inline assembly, `a.out` object files, and `Linux 0.12`'s `Makefile` hierarchy. This chapter provides the modern 64-bit ARM counterpart for **ABOS Lesson 20**: GNU AArch64 assembler (`gas`) syntax, freestanding C11 compilation flags and GCC inline assembly extensions, the **AAPCS64** procedure call standard, **ELF64** dual-linker-script relocation (`linker.ld` vs. `user/link.ld`), flat binary extraction + `USTAR` packaging, and ABOS's recursive `Makefile` build architecture.

---

## 3.1 GNU AArch64 Assembler (gas) Syntax and Directives

ABOS Lesson 20 contains **five assembly source files** totaling **445 lines**:

1. `kernel/core/start.S` (179 lines) — Bare-metal `.idmap` boot trampoline, `EL2 → EL1h` transition, early 1 GB block page tables, MMU/cache activation, `.bss` zeroing, and per-CPU stack initialization.
2. `kernel/core/exceptions.S` (130 lines) — 2 KB-aligned `VBAR_EL1` exception vector table and `regsave` / `regrestore` macros for the 288-byte `iframe_t` trap frame.
3. `kernel/core/switch.S` (42 lines) — `arm64_context_switch()` saving and restoring the 112-byte `context_switch_frame_t` across kernel stacks.
4. `kernel/core/uspace_entry.S` (50 lines) — `arm64_uspace_entry()` zeroing general-purpose registers to prevent kernel data leaks and entering `EL0t` via `eret`.
5. `user/ulibc/crt0.S` (44 lines) — Standalone `EL0` user-space entry `_start` at `0x20000000`, zeroing user `.bss`, calling `__ulibc_init()` and `main()`, and invoking `sys_process_exit()`.

Notice that every assembly file uses the **uppercase `.S` extension** rather than lowercase `.s`. When invoked via `aarch64-linux-gnu-gcc -c foo.S` (`config.mk:62-64`), GCC runs the **C Preprocessor (`cpp`)** prior to `gas`, enabling `#define` constants (such as `#define IFRAME_SIZE 288` in `exceptions.S`) and C-style `/* ... */` block comments.

### 3.1.1 AArch64 Instruction Syntax Conventions

Unlike x86 AT&T syntax (where `movl %eax, %ebx` moves left-to-right), standard **ARM64 (`AArch64`) assembly** places the **destination register on the left** for data-processing and load instructions (`op dst, src1, src2`), and the **value register on the left** for store instructions (`str src_reg, [base_addr, #offset]`):

```asm
mov     x20, x0                 /* x20 = x0 (64-bit register move) */
sub     x10, x10, x9, lsl #13   /* x10 = x10 - (x9 << 13) [8 KB per CPU] */
stp     x0,  x1,  [sp, #16]     /* Store Pair: Mem[sp+16]=x0, Mem[sp+24]=x1 */
stp     xzr, xzr, [x10], #16    /* Post-index: Mem[x10..x10+15]=0; x10 += 16 */
ldp     x29, x30, [sp, #96]     /* Load Pair:  x29=Mem[sp+96], x30=Mem[sp+104] */
```

Every general-purpose register can be referenced either as a **64-bit X-register (`x0..x30`)** or as its **lower 32-bit W-register (`w0..w30`)**. Writing to any `wN` register architectural-automatically **zero-extends** bits `[63:32]` of `xN`! Register 31 has two context-sensitive encodings: **`xzr`/`wzr`** (hardwired zero register in arithmetic/load/store value positions) versus **`sp`/`wsp`** (stack pointer in base-address and explicit `SP` arithmetic instructions).

### 3.1.2 Essential `gas` Assembler Directives Used in ABOS

| Directive | Example in ABOS | Meaning & Pitfalls to Watch For |
| :--- | :--- | :--- |
| `.section name, "flags"` | `.section .idmap.text, "ax"` | Switches output section; `"a"` = allocatable in memory, `"x"` = executable, `"w"` = writable. |
| `.global sym` | `.global arm64_context_switch` | Exports `sym` in the `ELF` symbol table so C files and linker scripts can bind to it. |
| `.type sym, %function` | `.type _start, %function` | Marks symbol type `STT_FUNC` in `ELF` metadata (`crt0.S:15`) for clean GDB disassembly. |
| `.equ NAME, expr` | `.equ MAIR_VALUE, 0xFF00` | Defines an assembly-time symbolic constant (`start.S:8-12`). |
| `.balign bytes` | `.balign 4096` | Pads location counter to an **exact byte multiple** (`4096` bytes for page tables in `start.S:130`). |
| `.align power` | `.align 11` / `.align 7` | **Critical ARM `gas` rule**: On AArch64, `.align N` aligns to **`2^N` bytes**, NOT `N` bytes! Thus `.align 11` = `2¹¹ = 2,048` bytes (`VBAR_EL1` table alignment in `exceptions.S:66`), and `.align 7` = `2⁷ = 128` bytes (`0x80` vector slot stride in `exceptions.S:70-91`). |
| `.word val` / `.space N` | `.space 4096` | Emits a 32-bit integer (`.word 0`) or reserves `N` zero-initialized bytes (`.space 4096`). |
| `.macro name, args` | `.macro regsave, flags` | Defines a parameterized assembly macro expanded inline up to `.endm` (`exceptions.S:10-35`). |

### 3.1.3 Local Labels (`1:` / `1b` / `1f`) and `adr` vs. `ldr x0, =sym`

Two addressing idioms in `start.S` deserve special attention because misunderstanding them causes immediate bare-metal boot crashes:

1. **Numeric Local Labels (`1:`, `1b`, `1f`) vs. `.L` Local Labels**:
   - Labels prefixed with `.L` (such as `.Ldrop_el2` or `.Lbss_loop`) are file-local symbols omitted from the final `ELF` symbol table.
   - Single-digit numeric labels (`1:`, `2:`, `3:`) can be reused repeatedly inside the same file or macro: `b 1b` branches to the nearest preceding (`backward`) `1:` label, while `b.hs 2f` branches to the nearest following (`forward`) `2:` label (`crt0.S:24-29`).
2. **PC-Relative `adr` vs. Literal-Pool `ldr x0, =sym` During MMU Transition**:
   - **`adr x0, idmap_table` (`start.S:54, 102`)** adds a signed 21-bit PC-relative offset (`±1 MB`) to the *current hardware `PC`*. When CPU 0 is executing at physical address `0x400800xx` with the `MMU` off, `adr` produces the **true physical address** (`0x40081000`) required by `TTBR0_EL1` and `TTBR1_EL1`!
   - **`ldr x0, =high_kernel_entry` (`start.S:122`)** loads a 64-bit constant from a PC-relative literal pool filled by the linker with the symbol's **linked Virtual Memory Address (`VMA`)**—namely `0xFFFFFFFF80083xxx`! Executing `ldr x0, =high_kernel_entry` followed by `br x0` right after setting `SCTLR_EL1.M = 1` is the exact trampoline jump that transitions `PC` from low physical identity space (`TTBR0_EL1`) into higher-half kernel virtual space (`TTBR1_EL1`).

---

## 3.2 Freestanding C11 and GCC Extensions in ABOS

In `lesson_20/config.mk` (lines 50–52), both the `EL1` microkernel and all `EL0` programs are compiled with the following explicit flags:

```makefile
CFLAGS := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
          -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g \
          -I$(INC_DIR) -I$(ULIBC_DIR)
```

Every flag here serves a deliberate architectural purpose:

| Compiler Flag | Architectural Purpose |
| :--- | :--- |
| `-ffreestanding` | Instructs GCC that no hosted C library exists; only freestanding headers (`<stdint.h>`, `<stddef.h>`, `<stdarg.h>`) provided by the compiler itself are used, and `main()` has no special hosted assumptions. |
| `-nostdlib -nostartfiles` | Prevents the linker from pulling in glibc/newlib `crt1.o`, `libc.so`, or `libgcc` host startup glue; ABOS supplies `start.S` (for `EL1`) and `user/ulibc/crt0.S` + `libulibc.a` (for `EL0`). |
| **`-mgeneral-regs-only`** | **Crucial kernel safety flag**: Forbids GCC from generating floating-point or 128-bit NEON/SIMD instructions (`q0..q31` / `v0..v31`) for `memcpy`, struct zeroing, or `va_list` varargs! |
| `-fno-pie -fno-pic` | Disables Position-Independent Code and Global Offset Tables (`.got`), ensuring deterministic absolute/PC-relative addressing at `0xFFFFFFFF80083000+` (`EL1`) and `0x20000000` (`EL0`). |
| `-mcpu=cortex-a53` | Tunes instruction selection and valid system-register encodings for ARMv8.0-A Cortex-A53. |
| `-O0 -g -Wall -Wextra` | Preserves a 1-to-1 correspondence between C lines and machine instructions for effortless GDB single-stepping (`Chapter 17`), while enforcing strict compiler warnings. |

### 3.2.1 Why `-mgeneral-regs-only` Prevents Silent Trap Corruption

Why is `-mgeneral-regs-only` mandatory in an AArch64 kernel? Without `-mgeneral-regs-only`, GCC routinely optimizes struct assignments (`ab_port_packet_t pkt = {0};` or `va_start` in `dprintf()`) by emitting 128-bit `stp q0, q1, [...]` NEON vector instructions. Look back at `exceptions.S`: to keep syscalls and timer interrupts fast, `regsave` saves **only the 31 general-purpose registers (`x0..x30`)** (`288` bytes), *not* the thirty-two 128-bit `v0..v31` SIMD registers (`512` additional bytes). If an `EL1` IRQ or syscall handler silently used `q0`, it would clobber whatever `EL0` user calculation was in `q0`! Passing `-mgeneral-regs-only` guarantees at compile time that C code never touches `v0..v31`. (Meanwhile, `start.S:45-46` still sets `CPACR_EL1.FPEN = 0b11` so `EL0` user apps are free to use hardware FP/SIMD if desired.)

### 3.2.2 `volatile`, Layout Attributes, and `__atomic_*` Builtins

ABOS uses three essential C/GCC low-level mechanisms across hardware and SMP boundaries:

1. **`volatile` for MMIO & Cross-CPU Shared Flags**: Casting an MMIO address such as `(*(volatile uint32_t *)(GICD_BASE + 0x100))` tells the compiler that every read or write has observable hardware side effects—the compiler must emit an actual `ldr wN` or `str wN` instruction in program order and must never hoist, sink, or dead-store-eliminate the access.
2. **Type Attributes (`__attribute__((packed))` and `__attribute__((aligned(N)))`)**:
   - In `user/drivers/fs_srv.c` (lines 31–61), DMA descriptors (`virtq_desc_t`, `virtq_avail_t`, `virtio_blk_req_t`) and `tar_header_t` carry `__attribute__((packed))` so their byte offsets match the exact VirtIO and POSIX USTAR wire specifications with zero compiler padding.
   - In `kernel/mm/mmu.c` (`empty_ttbr0_l1`) and `heap.c` (`heap_block_t`), `__attribute__((aligned(4096)))` and `__attribute__((aligned(16)))` enforce hardware page-table and `AAPCS64` alignment invariants.
3. **GCC C11-Style Atomic Builtins**: For lock-free reference counting and cross-CPU signaling—such as `__atomic_fetch_add(&obj->ref_count, 1, __ATOMIC_SEQ_CST)` in `object.c` and `__atomic_load_n(&secondary_cpu_ready_count, __ATOMIC_ACQUIRE)` in `main.c`—GCC emits architectural ARMv8 load-acquire / store-release atomic loops automatically.

### 3.2.3 GCC Extended Inline Assembly (`__asm__ volatile`)

Whenever C code must read/write ARMv8 system registers (`MRS`/`MSR`), issue barriers (`DSB`/`ISB`), flush TLBs (`TLBI`), or execute `SVC #0` / `HVC #0`, ABOS uses GCC extended inline assembly:

```
__asm__ volatile (
    "assembler template"
    : output operands      /* optional */
    : input operands       /* optional */
    : clobbered resources  /* optional */
);
```

Consider the exclusive spinlock acquisition in `include/spinlock.h` (lines 57–66):

```c
__asm__ volatile(
    "sevl\n"
    "1: wfe\n"
    "ldaxr %[temp], [%[lock]]\n"
    "cbnz  %[temp], 1b\n"
    "stxr  %w[temp], %[val], [%[lock]]\n"
    "cbnz  %w[temp], 1b\n"
    : [temp] "=&r"(temp)
    : [lock] "r"(&lock->value), [val] "r"(val)
    : "cc", "memory");
```

Let us decode every constraint and modifier in this snippet:

- **`volatile`**: Forbids GCC from deleting or moving the assembly block even if its output variables appear unused.
- **`[temp] "=&r"(temp)`**: Named output operand `[temp]`. `"=r"` selects any general-purpose register; the **`&` ("earlyclobber")** modifier is critical! Because `temp` is written by `ldaxr` *before* input registers `[lock]` and `[val]` are read for the final time by `stxr`, `"=&r"` forbids GCC from allocating `temp` to the same physical register as `lock` or `val`!
- **`%w[temp]` vs. `%[temp]`**: On AArch64, `stxr` requires a **32-bit status destination register (`wN`)** (`0` = store succeeded, `1` = exclusive monitor lost). Prefixing `%w[...]` emits `wN` (e.g., `w2`), whereas `%[...]` emits the 64-bit `xN` (`x2`).
- **Explicit Register Binding (`register uint64_t x0 __asm__("x0")`)**: Used in `psci_cpu_on()` (`main.c:31-35`) and `__ab_syscall4()` (`ulibc.c:5-17`) where hardware/ABI protocols mandate exact physical registers (`x0..x3` and `x8`) across `hvc #0` or `svc #0`.
- **Clobber List (`"cc", "memory")`**: `"cc"` informs GCC that condition flags (`NZCV`) were modified; `"memory"` acts as a **compiler-level memory barrier**, forcing GCC to flush cached register values to RAM before the asm block and reload memory variables afterward.

---

## 3.3 AAPCS64 Calling Convention and C/Assembly Interworking

Every boundary between C and assembly in ABOS—`_start → lk_main`, `exceptions.S → arm64_sync_exception`, `sched_reschedule_locked → arm64_context_switch`, `user_thread_trampoline → arm64_uspace_entry`, and `crt0.S → __ulibc_init → main`—is governed by ARM's **Procedure Call Standard for the Arm 64-bit Architecture (`AAPCS64`)**.

Table 3-1 lists all 32 architectural registers and their exact roles in `AAPCS64` and ABOS:

| Register(s) | 32-bit View | AAPCS64 Role | Callee-Saved? | Specific Usage in ABOS Lesson 20 |
| :--- | :--- | :--- | :---: | :--- |
| **`x0 .. x7`** | `w0 .. w7` | Argument registers 1–8 & return values (`x0..x1`) | No (Caller) | Function arguments; `x0..x3` pass up to 4 ABOS syscall args; `x0` returns `ab_status_t`. |
| **`x8`** | `w8` | Indirect result location register | No (Caller) | **ABOS Syscall Number (`SYS_*`, `1..34`)** across `svc #0` (`syscalls.c:1049`). |
| **`x9 .. x15`** | `w9 .. w15` | Temporary scratch registers | No (Caller) | Used freely as scratch temps inside `start.S`, `exceptions.S`, and `switch.S`. |
| **`x16, x17`** | `w16, w17` | Intra-procedure-call scratch (`IP0`, `IP1`) | No (Caller) | Reserved for linker veneers / PLT stubs. |
| **`x18`** | `w18` | Platform Register | No (in Linux/ELF) | Saved in `iframe_t` across traps; zeroed on `EL0` entry. |
| **`x19 .. x28`** | `w19 .. w28` | **Callee-saved variables** | **Yes** | Preserved across calls! `start.S` stores `dtb_paddr` in `x20`; `crt0.S` stores `boot_ch` in `x19`; `switch.S` saves `x19..x28`. |
| **`x29` (`fp`)** | `w29` | Frame Pointer | **Yes** | Points to current stack frame record; saved in `switch.S`. |
| **`x30` (`lr`)** | `w30` | **Link Register** | **Yes** (by non-leaf) | Written with return PC (`PC + 4`) by `bl <func>`; consumed by `ret` (`br x30`). |
| **`sp`** | `wsp` | Hardware Stack Pointer (`SP_EL0` / `SP_EL1`) | **Yes** | **Must be 16-byte aligned (`SP % 16 == 0`) whenever used as a memory base!** |
| **`xzr`** | `wzr` | Hardwired Zero Register | N/A | Reads as `0`; discards writes (`stp xzr, xzr, ...` zeroes 16 bytes in one cycle). |

### 3.3.1 The Hardware 16-Byte Stack Alignment Rule

Unlike x86-64 (where 16-byte `RSP` alignment is merely a software ABI rule for SSE instructions), **ARMv8-A enforces 16-byte stack pointer alignment in silicon** (`SCTLR_EL1.SA` and `SCTLR_EL1.SA0`). If `sp` is not a multiple of 16 (`(sp & 0xF) != 0`) at the instant any load or store uses `sp` as its base register (`stp x29, x30, [sp, #-16]!`), the CPU immediately triggers a synchronous **SP Alignment Fault (`EC = 0x26`)**!

Look at how ABOS upholds this hardware invariant everywhere:
- `IFRAME_SIZE` in `exceptions.S` is **`288` bytes** (`18 × 16` bytes — `include/exceptions.h` explicitly includes `uint64_t pad` at offset `280` to round `280 → 288`!).
- `context_switch_frame_t` in `switch.S` is **`112` bytes** (`7 × 16` bytes: `tpidr_el0`, `tpidrro_el0`, `x19..x30`).
- Every boot stack (`_stack_top - cpu × 8192`), kernel thread stack (`sp_top & ~15ULL`), and `EL0` user stack (`0x00806000`) begins on a 4,096-byte page boundary (a multiple of 16).

### 3.3.2 Why `switch.S` Saves 112 Bytes While `exceptions.S` Saves 288 Bytes

Comparing `switch.S` and `exceptions.S` highlights the power of `AAPCS64`:
- **`arm64_context_switch(&prev->kernel_sp, next->kernel_sp)`** is invoked via a **synchronous C function call (`bl arm64_context_switch`)**. By `AAPCS64`, the calling C function (`sched_reschedule_locked`) has *already* saved any live caller-saved registers (`x0..x18`) it cares about! Therefore, `switch.S` only needs to push the **callee-saved registers (`x19..x30`) plus user TLS registers (`tpidr_el0`, `tpidrro_el0`)**—just **14 × 8 = 112 bytes**!
- Conversely, **an asynchronous hardware IRQ or `EL0` trap (`exceptions.S`)** can strike between *any* two instructions when `x0..x18` and `NZCV` flags (`SPSR_EL1`) are all live. Hence `regsave` must preserve **every** general-purpose register (`x0..x30`), `sp_el0`, `elr_el1`, `spsr_el1`, and `mdscr_el1` (**288 bytes**).

---

## 3.4 ELF64 Object Format, Linker Scripts and Flat Binary Images

Each `.c` or `.S` file compiles into a relocatable **`ELF64` (`Executable and Linkable Format`, `EM_AARCH64`)** `.o` object file containing four primary sections:

- **`.text`**: Executable machine instructions (`SHF_ALLOC | SHF_EXECINSTR`).
- **`.rodata`**: Read-only constants and string literals (`SHF_ALLOC`).
- **`.data`**: Initialized writable global/static variables (`SHF_ALLOC | SHF_WRITE`).
- **`.bss`**: Zero-initialized global/static variables (`SHT_NOBITS`, occupies **zero bytes** on disk in `.img`/`.bin` until cleared at runtime!).

### 3.4.1 Kernel Linker Script (`linker.ld`): Separating `VMA` and `LMA`

During boot, QEMU loads `image/kernel.img` as a contiguous flat byte stream starting at **physical address `0x40080000`**, yet once `start.S` enables the `MMU`, all kernel C functions must execute at **Higher-Half Virtual Addresses `0xFFFFFFFF80083000+`**. `lesson_20/linker.ld` solves this cleanly by separating each section's **Virtual Memory Address (`VMA`)** from its **Load Memory Address (`LMA`)** via the `AT(...)` directive:

```
Physical Load Address (LMA in RAM)             Linked Virtual Address (VMA in ELF)
----------------------------------             -----------------------------------
0x4008_0000  [ .idmap (12 KB: start.S +   ] -> 0x0000_0000_4008_0000  (VMA == LMA!)
             [  idmap_table + kernel_tbl) ]
0x4008_3000  [ .text   (EL1 code + uboot) ] -> 0xFFFF_FFFF_8008_3000  (AT 0x40083000)
0x4009_xxxx  [ .rodata (strings/const)    ] -> 0xFFFF_FFFF_8009_xxxx  (contiguous LMA)
0x4009_yyyy  [ .data   (initialized vars) ] -> 0xFFFF_FFFF_8009_yyyy  (contiguous LMA)
(NOBITS)     [ .bss + 32 KB Boot Stacks   ] -> 0xFFFF_FFFF_8009_zzzz .. _end
```

Because `.text` specifies `AT(ADDR(.idmap) + SIZEOF(.idmap))` (`0x40083000`) while `.` was advanced by `KERNEL_VA_OFF` (`0xFFFFFFFF40000000`), `objcopy -O binary kernel.elf kernel.img` emits a compact ~100 KB flat image with **zero multi-gigabyte gap** between `.idmap` and `.text`!

### 3.4.2 Shared User-Space Linker Script (`user/link.ld`)

Every standalone `EL0` server, shell, and application (`console-srv.bin`, `fs-srv.bin`, `shell.bin`, `calc.bin`) is linked with `lesson_20/user/link.ld` at **`USER_APP_BASE = 0x20000000`**:

1. `KEEP(*(.text._start))` places `_start` from `user/ulibc/crt0.S` at the **very first instruction (`0x20000000`)**, so jumping to `0x20000000` after mapping a `.bin` file into a `VMO`/`VMAR` always enters `_start` without needing an in-kernel ELF header parser!
2. Look closely at lines 45–47 of `user/link.ld`:
   ```ld
   .app_end ALIGN(8) : {
       LONG(0x0)
   }
   ```
   **Why is `.app_end` emitting a 4-byte `LONG(0x0)` *after* `.bss`?** Because `.bss` is `SHT_NOBITS`! Normally, `objcopy -O binary` strips trailing `SHT_NOBITS` sections from the output `.bin` file—which would cause the loader (`userboot` or `shell`) to allocate a `VMO` covering `.text/.rodata/.data` *without* allocating pages for `.bss`! Placing the 4-byte allocated `.app_end` section immediately after `__bss_end` forces `objcopy -O binary` to pad the `.bin` file across the entire `.bss` span so the mapped `VMO` automatically covers every global buffer (such as `console-srv`'s `vt_state_t` arrays)!

### 3.4.3 POSIX USTAR Archive Format (`bootfs.tar` and `disk.img`)

Both `image/bootfs.tar` (created by `image/make_bootfs.py`) and `image/disk.img` (created by `image/make_tar_disk.py`, padded to `1 MB`) use the standard **POSIX.1-1988 `USTAR` (`tarfile.USTAR_FORMAT`)** archive layout—a sequence of **512-byte blocks** requiring zero complex inode indirection:

```
+-------------------------------------------------------------------------+
| 512-Byte USTAR Header Block (tar_header_t):                             |
|   [0..99]    name[100]   : Null-terminated ASCII filename ("calc.bin")  |
|   [100..123] mode/uid/gid: ASCII octal permissions & IDs                |
|   [124..135] size[12]    : File size in ASCII OCTAL (parsed by oct2uint)|
|   [257..262] magic[6]    : "ustar\0"                                    |
+-------------------------------------------------------------------------+
| File Payload Sectors: ceil(file_size / 512) * 512 bytes                 |
+-------------------------------------------------------------------------+
| ... Next USTAR Entry ...                                                |
+-------------------------------------------------------------------------+
| End-of-Archive Marker: Two consecutive 512-byte all-zero blocks         |
+-------------------------------------------------------------------------+
```

Both `userboot.c` (walking `bootfs.tar` mapped in memory) and `fs_srv.c` (walking `tar_cache` DMA-read from `disk.img`) step from entry to entry with the 3-line formula `off += 512 + ((fsz + 511U) & ~511U)` until `th->name[0] == '\0'`.

---

## 3.5 Hierarchical Make Build System

Starting in Lesson 19 and perfected in Lesson 20, ABOS organizes its build into a clean, modular **12-Makefile hierarchy** rooted at `config.mk` and the top-level `Makefile`:

```
Makefile (top-level: check-tools -> kernel -> user -> image -> run)
  ├── config.mk (auto-detects CROSS_COMPILE & QEMU; defines CFLAGS & %.o rules)
  ├── kernel/Makefile
  │     ├── kernel/core/Makefile    -> start.o, exceptions.o, switch.o, uspace_entry.o,
  │     │                              dtb.o, exceptions_c.o, wait.o, thread.o,
  │     │                              object.o, syscalls.o, main.o
  │     ├── kernel/mm/Makefile      -> pmm.o, heap.o, mmu.o
  │     └── kernel/dev/Makefile     -> debuglog.o, gic.o, timer.o
  ├── user/Makefile
  │     ├── user/ulibc/Makefile     -> crt0.o, crt_env.o, ulibc.o -> libulibc.a
  │     ├── user/userboot/Makefile  -> userboot.o (only EL0 loader embedded in kernel.img)
  │     ├── user/drivers/Makefile   -> image/console-srv.bin, image/fs-srv.bin (@ 0x20000000)
  │     ├── user/shell/Makefile     -> image/shell.bin (@ 0x20000000)
  │     └── user/apps/Makefile      -> image/calc.bin (@ 0x20000000)
  └── image/Makefile
        ├── [1] Links EL1 kernel/*.o + userboot.o + ulibc.o -> kernel.elf -> image/kernel.img
        ├── [2] Runs make_bootfs.py   (console-srv.bin, fs-srv.bin, shell.bin) -> image/bootfs.tar
        └── [3] Runs make_tar_disk.py (motd.txt, zircon.txt, calc.bin)         -> image/disk.img
```

Two practical engineering details make this build system effortless across machines:
1. **Automatic Toolchain Discovery (`config.mk:12-28` & `install_tools.sh`)**: `config.mk` probes `aarch64-linux-gnu-gcc` (standard on Debian/Ubuntu/Fedora), `aarch64-elf-gcc` (Homebrew on macOS), and local prebuilt Zircon toolchains automatically; running `./install_tools.sh` installs all prerequisites in one step on Debian/Ubuntu, Fedora, Arch, or macOS.
2. **True Architectural Artifact Isolation (`image/Makefile`)**: Notice that `console_srv.o`, `fs_srv.o`, `shell.o`, and `calc.o` are **never** passed to `$(KERNEL_ELF)`! `kernel.img` contains solely the `EL1` microkernel plus `userboot`; system servers live exclusively inside `bootfs.tar`, and user files/apps live exclusively inside `disk.img`.

---

## 3.6 Summary

ABOS Lesson 20 is built with standard, transparent GNU tools: `gas` for the 445 lines of hardware entry/exit assembly, freestanding C11 with `-mgeneral-regs-only` to guarantee SIMD-free `EL1` trap safety, GCC extended inline assembly for ARMv8 system registers and atomic spinlocks, `AAPCS64` with strict 16-byte hardware stack alignment, two clean linker scripts (`linker.ld` higher-half `VMA`/`LMA` vs. `user/link.ld` `@ 0x20000000`), and POSIX `USTAR` archives orchestrated by a modular recursive `Makefile` tree. With the toolchain in hand, Chapter 4 dives into the ARMv8-A processor architecture itself: privilege levels, Stage-1 4 KB page tables, ASIDs, exception vectors, and multi-core SMP synchronization.
