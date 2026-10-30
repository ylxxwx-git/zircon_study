# 13 User-Space Linker Script, C Runtime and Library (ulibc)

## 13.1 Main Functions

In the preceding chapters we traced the entire `EL1` ABOS microkernel: boot assembly, 4-level `TTBR0_EL1`/`TTBR1_EL1` page tables, SMP scheduling, capability-checked kernel objects (`Dispatcher`s), asynchronous `Port`s, capability-passing `Channel`s, `VMO`/`VMAR` virtual memory management, `Job`/`Process`/`Thread` tasks, user-space driver objects (`Resource`, physical/contiguous `VMO`s, `InterruptDispatcher`), and the 34-entry `SVC #0` system call table. Starting in this chapter, we cross the exception boundary into unprivileged **`EL0` user space** and examine the runtime software stack that brings a microkernel userland to life.

Unlike a monolithic UNIX kernel — where `glibc` or `musl` wraps ambient-authority POSIX system calls (`open("/dev/tty0")`, `read(fd, ...)`, `fork()`, `execve()`) — an object-capability microkernel such as Google's Zircon or ABOS provides **no global kernel namespace, no file-descriptor table, and no superuser UID**. When a new `EL0` process begins execution at `_start`, the *only* authority it possesses in the entire universe is the set of capability handles explicitly transferred into its handle table over a bootstrap `Channel`. Consequently, ABOS's user-space C runtime and library (**`ulibc`**) fulfills six foundational responsibilities:

1. **Deterministic Flat-Binary Address Layout (`user/link.ld`)**: Links every standalone `EL0` system server (`console-srv.bin`, `fs-srv.bin`), interactive shell (`shell.bin`), and user application (`calc.bin`) at virtual base **`0x20000000`**, placing `_start` at byte offset `0` and anchoring `.bss` with a trailing `PROGBITS` `.app_end` word so raw flat binaries (`objcopy -O binary`) map cleanly with zero ELF-loader bloat in the kernel.
2. **Assembly Startup & `.bss` Zeroing (`user/ulibc/crt0.S`)**: Preserves the two register arguments `(x0 = boot_ch, x1 = arg1)` injected by `SYS_PROCESS_START`, zeroes `[__bss_start .. __bss_end)` using 64-bit `xzr` stores, initializes the C capability environment via `__ulibc_init`, invokes `main()`, and forwards `main()`'s return code to `sys_process_exit()`.
3. **Zircon `processargs`-Style Capability Unpacking (`user/ulibc/crt_env.c`)**: Reads the startup `ab_proc_args_t` message (`magic = "ABPA"`) and up to 8 typed capability handles (`AB_PA_LOG`, `AB_PA_VMAR_ROOT`, `AB_PA_CONSOLE`, `AB_PA_FS`, `AB_PA_JOB_DEFAULT`, `AB_PA_RESOURCE`, `AB_PA_CHANNEL_SRV`) from `boot_ch`, closes `boot_ch` immediately to reclaim kernel IPC memory, and wires `printf()`, `u_gets()`, and `dlog_printf()` automatically to their capability endpoints.
4. **AArch64 `SVC #0` Syscall Gateway & 34 Typed Stubs (`user/ulibc/ulibc.h`, `ulibc.c`)**: Marshals system call numbers into `x8` and parameters/descriptor pointers into `x0..x3` across `svc #0` with strict compiler memory barriers (`"memory"` clobber).
5. **Zero-Syscall User-Space Monotonic Clock (`ulibc_clock_get_monotonic`)**: Reads the ARMv8 Generic Timer physical counter (`cntpct_el0`) and frequency (`cntfrq_el0`) directly in `EL0` (unlocked by `CNTKCTL_EL1.EL0PCTEN = 1` in `EL1`) with `ISB` serialization and overflow-safe nanosecond conversion — mirroring Zircon's VDSO `zx_clock_get_monotonic`.
6. **Freestanding Formatting & Synchronous Console IPC (`u_vsnprintf`, `shell_print`, `shell_readline`, `u_format_size`)**: Implements a floating-point-free (`-mgeneral-regs-only`) formatting engine, chunked request/ACK console output over `Channel`s, blocking line input, and Zircon's `format_size_fixed()` human-readable memory formatter.

### Virtual Address Space of Every Standalone EL0 Process

Because every process owns an independent `vm_aspace_t` backed by its own `TTBR0_EL1` L1 page table and hardware 16-bit ASID, all standalone `EL0` executables share the exact same virtual memory map without colliding:

```text
  0x80000000 +-----------------------------------------------------------+
             |  End of 2 GB User VmAspace (TTBR0_EL1, T0SZ = 33)         |
  0x40000000 +-----------------------------------------------------------+
             |                                                           |
             |  Standalone Executable VMO Mapping (RWX)                  |
             |    0x20000000 : _start (.text._start -> .text -> .rodata  |
             |                 -> .data -> .bss -> .app_end)             |
             |    Mapped via sys_vmar_map(vmar_h, 0x10000000, ...)       |
  0x20000000 + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
             |  Dynamic First-Fit VMAR Allocations (vmar_offset = 0)     |
             |    e.g., PL011 MMIO (4K), VirtIO MMIO (16K),              |
             |    VirtIO Contiguous DMA (128K), or 'cat' file VMOs       |
             |    (separated by 4 KB unmapped guard gaps)                |
  0x10000000 +-----------------------------------------------------------+
             |  (Note: only kernel-embedded 'userboot' maps .text/.rodata|
             |   below 0x20000000 at USER_KVA_TO_UVA = 0x00081000)       |
  0x00806000 +-----------------------------------------------------------+
             |  24 KB EL0 User Stack (6 pages, RW, grows downward)       |
             |    Initial SP_EL0 = 0x00806000 (allocated when sp == 0)   |
  0x00800000 +-----------------------------------------------------------+
             |  Unmapped Lower Guard Region                              |
  0x00001000 +-----------------------------------------------------------+
             |  4 KB NULL-Pointer Trap Page (unmapped)                   |
  0x00000000 +-----------------------------------------------------------+
```

### Summary of Files in Chapter 13

| Source File | Lines | Build Artifact | Role in ABOS User Space |
| :--- | :---: | :--- | :--- |
| `user/link.ld` | 54 | *(Linker Script)* | Places `_start` at `0x20000000` and pads `.bss` via `.app_end` for flat `.bin` images |
| `user/ulibc/crt0.S` | 44 | `crt0.o` (`libulibc.a`) | `EL0` entry `_start`: saves `(x0, x1)`, zeroes `.bss`, runs `__ulibc_init` -> `main` -> `sys_process_exit` |
| `user/ulibc/ulibc.h` | 125 | *(Public Header)* | Declares all 34 wrapped ABOS syscalls, startup handle APIs, `printf`/`dlog_printf`, and string helpers |
| `user/ulibc/crt_env.c` | 122 | `crt_env.o` (`libulibc.a`) | Unpacks `ab_proc_args_t` + `AB_PA_*` handles from `boot_ch` and routes standard console/debuglog I/O |
| `user/ulibc/ulibc.c` | 534 | `ulibc.o` (`libulibc.a` & `kernel.elf`) | Implements `__ab_syscall4` (`svc #0`), 34 syscall wrappers, `cntpct_el0` clock, console RPC, and `u_vsnprintf` |

> **Why is `ulibc` split into `crt0.S` + `crt_env.c` vs `ulibc.c`?** Notice in `image/Makefile` that `kernel.elf` links `user/userboot/userboot.o` together with **only** `user/ulibc/ulibc.o` (omitting `crt0.o` and `crt_env.o`). Because `userboot` is embedded inside the kernel image and mapped read/execute into `EL0` from kernel physical pages, `userboot` cannot write to static `.bss` globals in `crt_env.c`; instead, `userboot` keeps all state on its private 24 KB `EL0` stack while still sharing every `sys_*` stub, `ulog()`, and `u_snprintf()` from `ulibc.o`! All standalone `BOOTFS` and TAR-disk binaries (`console-srv.bin`, `fs-srv.bin`, `shell.bin`, `calc.bin`) link the full `crt0.o` + `libulibc.a` archive into their own private writable VMOs at `0x20000000`.

## 13.2 user/link.ld

### 13.2.1 Functional Description

`user/link.ld` is the unified GNU `ld` linker script used by `user/drivers/Makefile`, `user/shell/Makefile`, and `user/apps/Makefile` (invoked with `USER_LDFLAGS := -n -T user/link.ld`) to link every standalone `EL0` executable in ABOS. Its job is to produce a self-contained, position-fixed executable image starting at virtual address `0x20000000` that can be converted directly into a flat binary (`objcopy -O binary`) and mapped straight into a child process's VMAR without requiring an in-kernel ELF parser.

### 13.2.2 Heavily Commented Source Code

#### Program 13-1: `lesson_20/user/link.ld`

<!-- ECLK_CODE_LISTING file="lesson_20/user/link.ld" start="1" end="54" total="54" -->
```ld
       //// File header block: documents the universal EL0 linker script shared by every
       //// standalone user-space binary in ABOS Lesson 20 (`console-srv.bin`,
       //// `fs-srv.bin`, `shell.bin`, and `calc.bin`). Only the kernel-embedded root
       //// bootstrap task (`userboot`) is linked via the kernel's `linker.ld` rather than
       //// `user/link.ld`.
   1 | /* =========================================================================
   2 |  * user/link.ld - Common Linker Script for All ABOS EL0 User-Space Executables
   3 |  *
   4 |  * Used by all standalone EL0 user-space servers, shells, and applications:
   5 |  *   - user/drivers/console_srv.c -> image/console-srv.bin
   6 |  *   - user/drivers/fs_srv.c      -> image/fs-srv.bin
   7 |  *   - user/shell/shell.c         -> image/shell.bin
   8 |  *   - user/apps/*.c              -> image/<app>.bin
   9 |  *
  10 |  * Virtual Address Layout:
  11 |  *   0x20000000 : _start (defined in user/ulibc/crt0.S, section .text._start)
  12 |  *                followed by .text, .rodata, .data, and .bss
  13 |  * ========================================================================= */
  14 | 
       //// Target the 64-bit ARMv8-A (`aarch64`) ELF architecture and declare `_start`
       //// (defined in `user/ulibc/crt0.S`) as the canonical ELF entry symbol
       //// (`e_entry`). Even though the build pipeline subsequently strips the linked
       //// `.elf` into a headerless flat `.bin` image via `aarch64-elf-objcopy -O
       //// binary`, declaring `ENTRY(_start)` suppresses linker warnings and records the
       //// exact entry address in `image/*.elf` for GDB/objdump inspection.
  15 | OUTPUT_ARCH(aarch64)
  16 | ENTRY(_start)
  17 | 
       //// Every standalone EL0 process links at virtual base `0x20000000` (512 MB).
       //// Inside the kernel, each process's dynamic root VMAR (`vmar-dynamic`) spans
       //// `[0x10000000 .. 0x40000000)`, so passing `vmar_offset = 0x10000000` to
       //// `sys_vmar_map()` deterministically places byte 0 of the executable VMO at
       //// `0x10000000 + 0x10000000 = 0x20000000`. Meanwhile, dynamic `vmar_offset = 0`
       //// mappings (MMIO pages, DMA buffers, opened files) start at `0x10000000` and
       //// grow upward, leaving 256 MB of collision-free virtual space below the program
       //// image.
  18 | USER_APP_BASE = 0x20000000;
  19 | 
       //// Begin output section layout by setting the location counter `.` to
       //// `0x20000000`.
  20 | SECTIONS
  21 | {
  22 |     . = USER_APP_BASE;
  23 | 
       //// `.text` output section: `KEEP(*(.text._start))` is mandatory for flat-binary
       //// executables! Because `objcopy -O binary` discards the ELF header, the parent
       //// spawner (`userboot` or `shell`) always jumps directly to the base of the
       //// mapped VMO (`0x20000000`). Placing `.text._start` ahead of `*(.text .text.*)`
       //// and wrapping it in `KEEP(...)` guarantees that `_start` resides at exact byte
       //// offset `0x0` of the `.bin` payload.
  24 |     .text : {
  25 |         KEEP(*(.text._start))
  26 |         *(.text .text.*)
  27 |     }
  28 | 
       //// `.rodata` output section: 8-byte aligned constant data (string literals,
       //// format strings, lookup tables such as `srv_types` and `units`).
  29 |     .rodata ALIGN(8) : {
  30 |         *(.rodata .rodata.*)
  31 |     }
  32 | 
       //// `.data` output section: 8-byte aligned initialized read-write global and
       //// `static` variables.
  33 |     .data ALIGN(8) : {
  34 |         *(.data .data.*)
  35 |     }
  36 | 
       //// `.bss` output section: 8-byte aligned zero-initialized static storage
       //// (including `crt_env.c`'s startup handle table) plus `*(COMMON)` tentative
       //// definitions. Both `__bss_start` and `__bss_end` are aligned to 8-byte
       //// boundaries so `crt0.S` can zero `.bss` using 64-bit `str xzr, [x2], #8` stores
       //// without overrunning or misaligning.
  37 |     .bss ALIGN(8) : {
  38 |         __bss_start = .;
  39 |         *(.bss .bss.*)
  40 |         *(COMMON)
  41 |         . = ALIGN(8);
  42 |         __bss_end = .;
  43 |     }
  44 | 
       //// Subtle bare-metal binary trick — `.app_end`: by default, `objcopy -O binary`
       //// omits trailing `NOBITS` sections such as `.bss` from the output `.bin` file.
       //// If `.bss` crossed a 4 KB page boundary beyond `.data`, a spawner that rounds
       //// only the `.bin` file size up to `PAGE_SIZE` would leave the upper `.bss` pages
       //// unmapped in the child's VMAR, causing a fatal Translation Fault in `crt0.S`!
       //// Emitting a single 4-byte `PROGBITS` word (`LONG(0x0)`) immediately *after*
       //// `.bss` forces `objcopy -O binary` to pad the flat image all the way across
       //// `[__bss_start .. __bss_end)`, guaranteeing that the VMO created by `userboot`
       //// or `fs-srv` backs every byte of `.bss`.
  45 |     .app_end ALIGN(8) : {
  46 |         LONG(0x0)
  47 |     }
  48 | 
       //// Discard compiler version strings (`.comment`), ELF build-ID/ABI notes
       //// (`.note*`), and C++/DWARF unwind tables (`.eh_frame*`) to keep the flat BOOTFS
       //// and TAR disk binaries compact.
  49 |     /DISCARD/ : {
  50 |         *(.comment)
  51 |         *(.note*)
  52 |         *(.eh_frame*)
  53 |     }
  54 | }
       //// End of `user/link.ld`: all standalone EL0 binaries share this single 54-line
       //// layout because hardware MMU + ASID isolation gives each process its own
       //// private `TTBR0_EL1` translation tables at `0x20000000`.
```

### 13.2.3 Technical Notes

1. **Why `-n` (`--nmagic`) in `USER_LDFLAGS`?** By default, GNU `ld` aligns `PT_LOAD` segments to 64 KB (`0x10000`) on AArch64 so ELF loaders can map `.text` and `.data` with separate page permissions even on 64 KB-page kernels. Passing `-n` disables automatic 64 KB segment padding, keeping `console-srv.bin`, `fs-srv.bin`, `shell.bin`, and `calc.bin` compact (typically 8–20 KB each) so all three `BOOTFS` servers/shells fit comfortably in a tiny RAM disk and `disk.img` fits inside `fs-srv`'s 116 KB DMA cache.
2. **Why `.app_end` (`LONG(0x0)`) is Critical for Flat Binaries**: In ELF, `.bss` has section type `SHT_NOBITS` — it occupies virtual address space (`p_memsz`) but zero bytes in the file (`p_filesz`). When `objcopy -O binary` flattens an ELF file, it strips all trailing `SHT_NOBITS` sections. If a server such as `console-srv` declares a `vt_state_t` or global array in `.bss` that crosses into a new 4 KB page, omitting `.bss` from the `.bin` file would cause `spawn_from_bootfs` to create a VMO one page too small! Placing `.app_end` (`SHT_PROGBITS`, 4 zero bytes) *after* `.bss` forces `objcopy` to materialize the entire `[__bss_start .. __bss_end)` span inside the `.bin` file so `map_sz` always covers `.bss`.
3. **Code Model (`-fno-pie -fno-pic`)**: `config.mk` compiles user code with `-fno-pie -fno-pic` under GCC's default `small` code model (`ADRP` + `ADD`/`LDR`, +/-4 GB PC-relative reach), allowing zero-GOT direct symbol addressing anywhere around `0x20000000`.

---

## 13.3 user/ulibc/crt0.S

### 13.3.1 Functional Description

`user/ulibc/crt0.S` implements `_start`, the first instruction executed in `EL0` by every standalone ABOS server, shell, and user application. When a parent calls `sys_process_start(proc_h, thr_h, 0x20000000, 0, c_ch, arg2)`, the kernel allocates a 24 KB `EL0` stack (`[0x00800000 .. 0x00806000)`), installs `c_ch` into the child's handle table, places that child handle ID into `x0` and `arg2` into `x1`, sets `ELR_EL1 = 0x20000000` and `SP_EL0 = 0x00806000`, zeroes `x2..x30`, and executes `eret` in `arm64_uspace_entry`. `_start` bridges that raw register state into a standard C runtime environment.

### 13.3.2 Heavily Commented Source Code

#### Program 13-2: `lesson_20/user/ulibc/crt0.S`

<!-- ECLK_CODE_LISTING file="lesson_20/user/ulibc/crt0.S" start="1" end="44" total="44" -->
```asm
       ## File header block: outlines the 5-step C runtime (`crt0`) startup contract
       ## between the ABOS kernel (`SYS_PROCESS_START` -> `arm64_uspace_entry`) and
       ## every standalone EL0 C program (`int main(void)`).
   1 | /* =========================================================================
   2 |  * user/ulibc/crt0.S - ABOS User-Space C Runtime Startup (crt0)
   3 |  *
   4 |  * _start: Entry point for all standalone EL0 servers, shells, and apps
   5 |  *         linked with user/link.ld at VA 0x20000000.
   6 |  *   1. Receives (boot_ch, arg1) in (x0, x1) from SYS_PROCESS_START
   7 |  *   2. Zeroes the .bss section ([__bss_start .. __bss_end))
   8 |  *   3. Initializes startup handles via __ulibc_init(boot_ch, arg1)
   9 |  *   4. Invokes int main(void)
  10 |  *   5. Passes main()'s return code to sys_process_exit(retcode)
  11 |  * ========================================================================= */
  12 | 
       ## Place `_start` in its own dedicated executable, allocatable section
       ## (`.text._start`, flags `"ax"`) so `user/link.ld`'s `KEEP(*(.text._start))`
       ## pins the first instruction of `_start` at `0x20000000`.
  13 | .section .text._start, "ax"
  14 | .global _start
  15 | .type _start, %function
       ## Entry point `_start`: when the kernel's `arm64_uspace_entry` executes `eret`
       ## into `EL0t`, hardware state is already clean — `SP_EL0` points to the top of
       ## the 24 KB user stack (`0x00806000`), `x2..x30` are zeroed to prevent kernel
       ## pointer leaks, `x0` holds the transferred child endpoint handle `boot_ch` (or
       ## `0` if none), and `x1` holds the 64-bit integer argument `arg1` (such as the
       ## Virtual Terminal index `0..3` for `abos-shell`).
       ##
       ## Save `x0` and `x1` into AAPCS64 callee-saved registers `x19` and `x20` so
       ## their values survive temporary register usage and future helper calls.
  16 | _start:
  17 |     /* Preserve startup arguments (x0 = boot_ch, x1 = arg1) */
  18 |     mov     x19, x0
  19 |     mov     x20, x1
  20 | 
       ## Zero `.bss` across `[__bss_start, __bss_end)`: load the linker-script bounds
       ## via literal-pool pseudo-instructions `ldr x2, =__bss_start` and `ldr x3,
       ## =__bss_end`, then write 8 zero bytes per iteration (`str xzr, [x2], #8`) until
       ## `x2 >= x3` (`b.hs 2f`, unsigned higher-or-same). Even though `.app_end` causes
       ## `objcopy` to pad `.bss` with zeroes in the image file, explicitly clearing
       ## `.bss` in `crt0.S` upholds the C99 runtime invariant even if a VMO were ever
       ## mapped copy-on-write or reused.
  21 |     /* Zero .bss ([__bss_start .. __bss_end)) */
  22 |     ldr     x2, =__bss_start
  23 |     ldr     x3, =__bss_end
  24 | 1:
  25 |     cmp     x2, x3
  26 |     b.hs    2f
  27 |     str     xzr, [x2], #8
  28 |     b       1b
  29 | 2:
  30 | 
       ## Restore `(boot_ch, arg1)` from `(x19, x20)` into `(x0, x1)` and call
       ## `__ulibc_init(boot_ch, arg1)` in `crt_env.c`. Before `main()` runs a single
       ## line of application code, `__ulibc_init` reads the `ab_proc_args_t` message
       ## from `boot_ch`, populates the local startup capability table, binds `printf()`
       ## and `dlog_printf()` to their `AB_PA_CONSOLE` and `AB_PA_LOG` handles, and
       ## closes `boot_ch`.
  31 |     /* Initialize ulibc startup environment: __ulibc_init(boot_ch, arg1) */
  32 |     mov     x0, x19
  33 |     mov     x1, x20
  34 |     bl      __ulibc_init
  35 | 
       ## Invoke the application or system server's `int main(void)` entry function.
  36 |     /* Call application/server int main(void) */
  37 |     bl      main
  38 | 
       ## Under AAPCS64, `main()` returns its 32-bit `int` exit status in `w0` (`x0`).
       ## Calling `bl sys_process_exit` forwards `x0` directly as the first argument to
       ## `sys_process_exit(int64_t retcode)`, which issues `SYS_PROCESS_EXIT` to tear
       ## down the process, close all remaining handles, and assert
       ## `AB_SIGNAL_TASK_TERMINATED` to wake any waiting parent (`userboot` or
       ## `shell`).
  39 |     /* Pass return value of main (in x0) to sys_process_exit(retcode) */
  40 |     bl      sys_process_exit
  41 | 
       ## Defensive unreachable park loop (`wfe; b 3b`): `sys_process_exit` never
       ## returns from `EL1`, so execution never reaches label `3:`.
  42 | 3:
  43 |     wfe
  44 |     b       3b
```

### 13.3.3 Technical Notes

1. **AAPCS64 Register Preservation (`x19`, `x20`)**: Under the Procedure Call Standard for the Arm 64-bit Architecture (AAPCS64), `x0..x18` are caller-saved/temporary registers, whereas `x19..x28` are callee-saved. Storing `(boot_ch, arg1)` in `(x19, x20)` at entry guarantees they remain intact across `.bss` initialization (and would remain intact even if another early hook were called prior to `__ulibc_init`).
2. **16-Byte Stack Alignment Invariant**: AArch64 hardware faults (`SP Alignment Fault`) if `SP_EL0` is dereferenced when not 16-byte aligned (`SP % 16 != 0`). `user_thread_trampoline` sets `SP_EL0 = 0x00806000` (4 KB and 16-byte aligned), and `_start` never modifies `sp` before `bl __ulibc_init`, satisfying AAPCS64's 16-byte entry alignment requirement.
3. **Graceful Return from `main()`**: Because `main()` returns its `int` result in `w0`/`x0` and `sys_process_exit(int64_t retcode)` expects its sole parameter in `x0`, line 40 (`bl sys_process_exit`) needs only a sign-extended/compatible value — note that in C, `return -1` in `int main(void)` places `0xFFFFFFFF` in `w0` (upper 32 bits zeroed in `x0`), or small non-negative status `0` in `x0`, cleanly terminating the process and waking any `sys_object_wait_one(proc_h, AB_SIGNAL_TASK_TERMINATED, ...)` waiter.

---

## 13.4 user/ulibc/ulibc.h

### 13.4.1 Functional Description

`user/ulibc/ulibc.h` is the single public SDK header included by all ABOS user-space components (`#include "ulibc.h"`). It brings together the shared kernel UAPI headers (`object.h`, `syscalls.h`, `ipc_proto.h`), prototypes for all 34 wrapped system calls, startup-capability lookup functions, the `EL0` monotonic clock, `printf`/`dlog_printf` formatted I/O, explicit-handle console IPC helpers, and freestanding string/numeric utilities.

### 13.4.2 Heavily Commented Source Code

#### Program 13-3: `lesson_20/user/ulibc/ulibc.h`

<!-- ECLK_CODE_LISTING file="lesson_20/user/ulibc/ulibc.h" start="1" end="125" total="125" -->
```c
       //// Header guard and freestanding includes: `<stdarg.h>`, `<stddef.h>`, and
       //// `<stdint.h>` are compiler-supplied freestanding headers from GCC (requiring no
       //// host OS libc), while `object.h`, `syscalls.h`, and `ipc_proto.h` share exact
       //// UAPI type definitions (`ab_handle_t`, `ab_status_t`, `ab_rights_t`, `SYS_*`
       //// numbers, `console_msg_t`, `fs_msg_t`, `ab_proc_args_t`) between `EL1` and
       //// `EL0`.
   1 | /* =========================================================================
   2 |  * user/ulibc/ulibc.h - ABOS User-Space C Library & Wrapped System Calls
   3 |  * ========================================================================= */
   4 | #ifndef ABOS_ULIBC_H
   5 | #define ABOS_ULIBC_H
   6 | 
   7 | #include <stdarg.h>
   8 | #include <stddef.h>
   9 | #include <stdint.h>
  10 | #include "object.h"
  11 | #include "syscalls.h"
  12 | #include "ipc_proto.h"
  13 | 
       //// Low-level 4-argument AArch64 `SVC #0` trap primitive implemented in `ulibc.c`.
       //// All 34 user-space system call wrappers in ABOS funnel through `__ab_syscall4`.
  14 | /* Low-level SVC entry in ulibc.c */
  15 | ab_status_t __ab_syscall4(uint64_t a0, uint64_t a1, uint64_t a2,
  16 |                           uint64_t a3, uint64_t syscall_num);
  17 | 
       //// Group 1 — Core thread/process lifecycle & kernel debug logging:
       ////   - `sys_debuglog_write`: writes a diagnostic string via an
       ////     `AB_OBJ_TYPE_LOG` capability.
       ////   - `sys_nanosleep`: blocks the calling thread on the kernel timer queue.
       ////   - `sys_process_exit`: terminates all threads in the calling process and
       ////     signals `AB_SIGNAL_TASK_TERMINATED` on the `ProcessDispatcher`.
  18 | /* =========================================================================
  19 |  * 1. Wrapped ABOS System Calls (User-Space ulibc API)
  20 |  * ========================================================================= */
  21 | ab_status_t sys_debuglog_write(ab_handle_t log_h, uint32_t flags, const char *msg, size_t len);
  22 | ab_status_t sys_nanosleep(uint64_t ticks);
  23 | void        sys_process_exit(int64_t retcode);
  24 | 
       //// Group 2 — Capability handle lifecycle & introspection (modeled after
       //// `zx_handle_close`, `zx_handle_duplicate`, and `zx_object_get_info` with
       //// `ZX_INFO_HANDLE_BASIC`): allows a process to release capabilities, create
       //// rights-attenuated duplicates, or inspect a handle's KOID, type, and rights.
  25 | /* Handle & Object Management */
  26 | ab_status_t sys_handle_close(ab_handle_t h);
  27 | ab_status_t sys_handle_duplicate(ab_handle_t h, ab_rights_t rights, ab_handle_t *out_h);
  28 | ab_status_t sys_object_get_info(ab_handle_t h, ab_handle_basic_info_t *out_info);
  29 | 
       //// Group 3 — Kernel state signals, `EventDispatcher`, and `EventPairDispatcher`:
       //// supports user signals (`AB_USER_SIGNAL_0..3`), peer signaling across linked
       //// eventpairs, and synchronous single-object blocking via `sys_object_wait_one`.
  30 | /* Signals, Events & Synchronization */
  31 | ab_status_t sys_event_create(uint32_t options, ab_handle_t *out_h);
  32 | ab_status_t sys_eventpair_create(uint32_t options, ab_handle_t *out_h0, ab_handle_t *out_h1);
  33 | ab_status_t sys_object_signal(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask);
  34 | ab_status_t sys_object_signal_peer(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask);
  35 | ab_status_t sys_object_wait_one(ab_handle_t h, ab_signals_t signals,
  36 |                                 uint64_t deadline, ab_signals_t *out_observed);
  37 | 
       //// Group 4 — `PortDispatcher` asynchronous event multiplexing (modeled after
       //// Zircon's `zx_port_create`, `zx_port_queue`, `zx_port_wait`, and
       //// `zx_object_wait_async`): enables single-threaded EL0 servers (`console-srv`
       //// and `fs-srv`) to wait simultaneously on hardware IRQs, multiple client IPC
       //// channels, and nanosecond timeouts.
  38 | /* Ports (Async Event Multiplexing) */
  39 | ab_status_t sys_port_create(uint32_t options, ab_handle_t *out_port);
  40 | ab_status_t sys_port_queue(ab_handle_t port_h, const ab_port_packet_t *pkt);
  41 | ab_status_t sys_port_wait(ab_handle_t port_h, uint64_t deadline, ab_port_packet_t *out_pkt);
  42 | ab_status_t sys_object_wait_async(ab_handle_t obj_h, ab_handle_t port_h,
  43 |                                   uint64_t key, ab_signals_t signals);
  44 | 
       //// Group 5 — Bidirectional `ChannelDispatcher` IPC (`sys_channel_create`,
       //// `sys_channel_write`, `sys_channel_read`): carries both up to 256 bytes of
       //// message data AND up to 8 capability handles (`MessagePacket`), atomically
       //// moving capabilities out of the sender's handle table and into the receiver's.
  45 | /* Channels (Message + Handle Capability IPC) */
  46 | ab_status_t sys_channel_create(uint32_t options, ab_handle_t *out_ch0, ab_handle_t *out_ch1);
  47 | ab_status_t sys_channel_write(ab_handle_t ch_h, const void *bytes, uint32_t num_bytes,
  48 |                               const ab_handle_t *handles, uint32_t num_handles);
  49 | ab_status_t sys_channel_read(ab_handle_t ch_h, void *bytes, uint32_t buf_bytes,
  50 |                              ab_handle_t *handles, uint32_t buf_handles,
  51 |                              uint32_t *actual_bytes, uint32_t *actual_handles);
  52 | 
       //// Group 6 — Virtual memory management (`VmObjectDispatcher` and
       //// `VmAddressRegionDispatcher`): creates anonymous page-backed VMOs, reads/writes
       //// VMO bytes via the kernel physmap, and maps or unmaps VMOs into a target
       //// process's virtual address space (`sys_vmar_map`, `sys_vmar_unmap`).
  53 | /* Virtual Memory Objects (VMO) & Address Space Regions (VMAR) */
  54 | ab_status_t sys_vmo_create(uint64_t size, uint32_t options, ab_handle_t *out_vmo);
  55 | ab_status_t sys_vmo_read(ab_handle_t vmo_h, void *buf, uint64_t offset, size_t len);
  56 | ab_status_t sys_vmo_write(ab_handle_t vmo_h, const void *buf, uint64_t offset, size_t len);
  57 | ab_status_t sys_vmar_map(ab_handle_t vmar_h, uint64_t vmar_offset,
  58 |                          ab_handle_t vmo_h, uint64_t vmo_offset,
  59 |                          uint64_t len, uint32_t perms, vaddr_t *out_va);
  60 | ab_status_t sys_vmar_unmap(ab_handle_t vmar_h, vaddr_t addr, uint64_t len);
  61 | 
       //// Group 7 — Task creation & startup (`sys_process_create`, `sys_thread_create`,
       //// `sys_process_start`): notice there is NO POSIX `fork()` or `execve()`!
       //// Instead, a parent builds a blank address space (`out_proc`, `out_vmar`), maps
       //// an executable VMO into `out_vmar`, creates a thread, writes capabilities into
       //// a bootstrap channel, and starts the child cleanly via `sys_process_start`.
  62 | /* Tasks: Jobs, Processes & Threads */
  63 | ab_status_t sys_process_create(ab_handle_t job_h, const char *name,
  64 |                                ab_handle_t *out_proc, ab_handle_t *out_vmar);
  65 | ab_status_t sys_thread_create(ab_handle_t proc_h, const char *name, ab_handle_t *out_thr);
  66 | ab_status_t sys_process_start(ab_handle_t proc_h, ab_handle_t thr_h, uintptr_t entry,
  67 |                               uintptr_t sp, ab_handle_t arg_h, uintptr_t arg2);
  68 | 
       //// Group 8 — User-space device driver capabilities (Zircon DDK equivalents):
       //// every call here requires a valid `ResourceDispatcher` handle (`res_h`).
       //// Without `res_h`, an unprivileged process (such as `abos-shell` or `calc.bin`)
       //// is cryptographically/architecturally barred from mapping MMIO pages
       //// (`sys_vmo_create_physical`), allocating physically contiguous DMA memory
       //// (`sys_vmo_create_contiguous`), or binding GIC hardware interrupt vectors
       //// (`sys_interrupt_create`).
  69 | /* User-Space Driver Capabilities: Physical/Contiguous DMA VMOs & Hardware IRQs */
  70 | ab_status_t sys_vmo_create_physical(ab_handle_t res_h, paddr_t pa, uint64_t size,
  71 |                                     ab_handle_t *out_vmo);
  72 | ab_status_t sys_vmo_create_contiguous(ab_handle_t res_h, uint64_t size,
  73 |                                       ab_handle_t *out_vmo, paddr_t *out_pa);
  74 | ab_status_t sys_interrupt_create(ab_handle_t res_h, uint32_t vector, ab_handle_t *out_irq);
  75 | ab_status_t sys_interrupt_wait(ab_handle_t irq_h);
  76 | ab_status_t sys_interrupt_ack(ab_handle_t irq_h);
  77 | 
       //// Group 9 — Object naming & Zircon-style task/VM diagnostics (`ps` and `vmaps`):
       //// `sys_job_get_ps_info` and `sys_process_get_maps` require a `JobDispatcher`
       //// capability (`job_h`), scoping process/VM visibility to that job's subtree.
  78 | /* Lesson 17: Task & VM Introspection ('ps' & 'vmaps') */
  79 | ab_status_t sys_object_set_name(ab_handle_t h, const char *name);
  80 | ab_status_t sys_job_get_ps_info(ab_handle_t job_h, ab_task_info_entry_t *entries,
  81 |                                 uint32_t max_entries, uint32_t *actual_out);
  82 | ab_status_t sys_process_get_maps(ab_handle_t job_h, ab_koid_t proc_koid,
  83 |                                  ab_info_maps_t *maps, uint32_t max_maps,
  84 |                                  uint32_t *actual_out);
  85 | 
       //// Section 2 — C Runtime (`crt_env.c`) & monotonic clock declarations:
       //// `ulibc_get_startup_handle*` mirrors Zircon's `zx_take_startup_handle()`,
       //// letting processes retrieve capabilities either by positional slot (`0..7`) or
       //// semantic `AB_PA_*` type tag (`AB_PA_LOG`, `AB_PA_CONSOLE`, `AB_PA_FS`,
       //// `AB_PA_RESOURCE`, etc.). `ulibc_clock_get_monotonic()` reads the ARMv8
       //// physical counter directly in `EL0` without a kernel trap.
  86 | /* =========================================================================
  87 |  * 2. C Runtime Environment, Formatted Printf & Standard Helpers
  88 |  * ========================================================================= */
  89 | void        __ulibc_init(ab_handle_t boot_ch, uint64_t arg1);
  90 | int         main(void);
  91 | 
  92 | ab_handle_t ulibc_get_startup_handle(uint32_t index);
  93 | ab_handle_t ulibc_get_startup_handle_by_type(uint8_t pa_type);
  94 | uint64_t    ulibc_get_startup_arg1(void);
  95 | ab_handle_t ulibc_get_log_handle(void);
  96 | ab_handle_t ulibc_get_console_handle(void);
  97 | ab_time_t   ulibc_clock_get_monotonic(void);
  98 | 
       //// High-level standard console & debuglog I/O APIs for standalone applications
       //// (`calc.bin`), backed automatically by the startup handles unpacked in
       //// `__ulibc_init()`.
  99 | /* Standard Console & DebugLog I/O for standalone apps (initialized by crt0) */
 100 | void        u_puts(const char *s);
 101 | void        u_gets(char *out_buf, size_t max_len);
 102 | void        u_dlog(const char *msg);
 103 | int         printf(const char *fmt, ...);
 104 | int         dlog_printf(const char *fmt, ...);
 105 | 
       //// Explicit-handle formatting and synchronous console RPC helpers (`u_vsnprintf`,
       //// `u_snprintf`, `con_printf`, `ulog`, `shell_print`, `shell_readline`), used
       //// both internally by `crt_env.c` and directly by `userboot` and `abos-shell`.
 106 | /* Formatted string & explicit handle Console/DebugLog helpers */
 107 | int         u_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
 108 | int         u_snprintf(char *buf, size_t size, const char *fmt, ...);
 109 | int         con_printf(ab_handle_t con_ch, const char *fmt, ...);
 110 | void        ulog(ab_handle_t log_h, const char *msg);
 111 | void        shell_print(ab_handle_t con_ch, const char *text);
 112 | void        shell_readline(ab_handle_t con_ch, char *out_buf, size_t max_len);
 113 | 
       //// Freestanding memory, string, integer parsing, and human-readable byte-size
       //// formatting utilities (`memcpy`, `memset`, `u_strlen`, `u_strcmp`, `u_strncmp`,
       //// `u_strcpy`, `u_atoi`, `u_itoa`, `u_format_size`).
 114 | /* String & Number utilities */
 115 | void       *memcpy(void *dst, const void *src, size_t n);
 116 | void       *memset(void *dst, int c, size_t n);
 117 | size_t      u_strlen(const char *s);
 118 | int         u_strcmp(const char *a, const char *b);
 119 | int         u_strncmp(const char *a, const char *b, size_t n);
 120 | void        u_strcpy(char *dst, const char *src);
 121 | int64_t     u_atoi(const char *s, int *out_ok);
 122 | void        u_itoa(int64_t val, char *buf);
 123 | char       *u_format_size(char *str, size_t str_size, size_t bytes);
 124 | 
 125 | #endif /* ABOS_ULIBC_H */
```

### 13.4.3 Technical Notes

1. **Direct Mapping to Zircon's `libzircon` (`<zircon/syscalls.h>`)**: Every `sys_*` prototype in `ulibc.h` corresponds 1-to-1 with a Zircon system call (`zx_handle_close`, `zx_handle_duplicate`, `zx_object_get_info`, `zx_port_create`, `zx_port_wait`, `zx_object_wait_async`, `zx_channel_write`, `zx_channel_read`, `zx_vmo_create`, `zx_vmar_map`, `zx_vmar_unmap`, `zx_process_create`, `zx_process_start`, `zx_vmo_create_physical`, `zx_vmo_create_contiguous`, `zx_interrupt_create`, etc.).
2. **Capability-Explicit vs Ambient APIs**: Notice that `ulibc.h` offers *two* tiers of console/logging helpers:
   - **Capability-explicit APIs** (`ulog(log_h, ...)`, `con_printf(con_ch, ...)`, `shell_print(con_ch, ...)`, `shell_readline(con_ch, ...)`), preferred by system servers and shells (`userboot`, `console-srv`, `shell`) to keep authority flow explicit at every call site.
   - **Startup-bound convenience APIs** (`printf(...)`, `u_puts(...)`, `u_gets(...)`, `dlog_printf(...)`), preferred by leaf applications (`calc.c`) so ordinary C code runs naturally over inherited capabilities.

---

## 13.5 user/ulibc/crt_env.c

### 13.5.1 Functional Description

`user/ulibc/crt_env.c` implements the `EL0` startup capability unpacker (`__ulibc_init`), startup handle accessors (`ulibc_get_startup_handle*`), and the standard C I/O wrappers (`printf`, `dlog_printf`, `u_puts`, `u_gets`, `u_dlog`). Modeled directly after Zircon's `zx_proc_args_t` (`processargs`) protocol, a parent process creates a one-shot bootstrap `Channel`, writes an `ab_proc_args_t` struct (`magic = 0x41425041`, `"ABPA"`, plus an 8-byte `handle_types[]` array of `AB_PA_*` tags) together with up to 8 capability handles, closes the parent endpoint, and passes the child endpoint `boot_ch` as `arg_h` (`x0`) to `sys_process_start`. When the child starts, `__ulibc_init` drains `boot_ch` in a single `sys_channel_read` and immediately closes `boot_ch`.

### 13.5.2 Heavily Commented Source Code

#### Program 13-4: `lesson_20/user/ulibc/crt_env.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/ulibc/crt_env.c" start="1" end="122" total="122" -->
```c
       //// File header block: `crt_env.c` implements the user-space half of ABOS's
       //// process-launch protocol (modeled after Zircon's `processargs` protocol and
       //// `__libc_extensions_init`). It bridges raw kernel capability handles into
       //// ergonomic C library globals before `main()` begins.
   1 | /* =========================================================================
   2 |  * user/ulibc/crt_env.c - C Runtime Startup Environment for Standalone EL0 Binaries
   3 |  *
   4 |  * Unpacks the bootstrap Channel message passed in x0 to _start (crt0.S),
   5 |  * stores up to 8 startup capability handles with AB_PA_* type tags (modeling
   6 |  * Zircon's processargs / zx_take_startup_handle), and provides standard
   7 |  * printf(), dlog_printf(), u_puts(), u_gets(), u_dlog().
   8 |  * ========================================================================= */
   9 | #include "ulibc.h"
  10 | 
       //// Process-private `.bss` state holding unpacked startup capabilities:
       ////   - `ULIBC_MAX_STARTUP_HANDLES` (8) matches `CHANNEL_MAX_MSG_HANDLES` (the
       ////     maximum number of handles carried by a single `ab_message_packet_t`).
       ////   - `__ulibc_handles[]` stores the local `ab_handle_t` values installed into
       ////     this process's handle table when the bootstrap channel is read.
       ////   - `__ulibc_handle_types[]` records each slot's `AB_PA_*` semantic tag.
       ////   - `__ulibc_log_h` and `__ulibc_con_ch` cache the default kernel debuglog
       ////     and `console-srv` IPC channel so `printf()` and `dlog_printf()` require
       ////     zero boilerplate in application code.
  11 | #define ULIBC_MAX_STARTUP_HANDLES 8U
  12 | 
  13 | static ab_handle_t __ulibc_handles[ULIBC_MAX_STARTUP_HANDLES];
  14 | static uint8_t     __ulibc_handle_types[ULIBC_MAX_STARTUP_HANDLES];
  15 | static uint32_t    __ulibc_num_handles = 0;
  16 | static uint64_t    __ulibc_arg1        = 0;
  17 | static ab_handle_t __ulibc_log_h       = AB_HANDLE_INVALID;
  18 | static ab_handle_t __ulibc_con_ch      = AB_HANDLE_INVALID;
  19 | 
       //// `__ulibc_init(boot_ch, arg1)`: invoked by `crt0.S` immediately after `.bss` is
       //// zeroed. Saves `arg1` (passed in `x1` by `SYS_PROCESS_START`, e.g., `vt_id` for
       //// `abos-shell`) and initializes all 8 handle slots to `AB_HANDLE_INVALID`.
  20 | void __ulibc_init(ab_handle_t boot_ch, uint64_t arg1) {
  21 |     __ulibc_arg1 = arg1;
  22 |     for (uint32_t i = 0; i < ULIBC_MAX_STARTUP_HANDLES; i++) {
  23 |         __ulibc_handles[i]      = AB_HANDLE_INVALID;
  24 |         __ulibc_handle_types[i] = AB_PA_NONE;
  25 |     }
  26 | 
       //// If the parent passed a valid bootstrap channel endpoint (`boot_ch != 0`), read
       //// the single startup `ab_proc_args_t` message and up to 8 attached capability
       //// handles in one atomic `sys_channel_read()` call, then immediately close
       //// `boot_ch` (`sys_handle_close`).
       ////
       //// Closing `boot_ch` right here is an important microkernel hygiene detail: the
       //// parent (`userboot` or `shell`) already closed its write end (`p_ch`) after
       //// enqueueing `pargs`, so closing `boot_ch` drops both channel endpoints'
       //// reference counts to zero and immediately frees the transient bootstrap
       //// `ChannelDispatcher` pair and `MessagePacket` back to the EL1 heap!
  27 |     if (boot_ch != AB_HANDLE_INVALID) {
  28 |         ab_proc_args_t pargs;
  29 |         memset(&pargs, 0, sizeof(pargs));
  30 |         uint32_t act_b = 0, act_h = 0;
  31 |         sys_channel_read(boot_ch, &pargs, sizeof(pargs),
  32 |                          __ulibc_handles, ULIBC_MAX_STARTUP_HANDLES,
  33 |                          &act_b, &act_h);
  34 |         sys_handle_close(boot_ch);
  35 |         __ulibc_num_handles = act_h;
  36 | 
       //// Validate the `ab_proc_args_t` header (`magic == 0x41425041`, ASCII `"ABPA"`).
       //// Walk each received handle, record its `AB_PA_*` type tag, and auto-discover
       //// the first `AB_PA_LOG` handle (`__ulibc_log_h`) and first `AB_PA_CONSOLE`
       //// handle (`__ulibc_con_ch`).
  37 |         if (act_b >= sizeof(ab_proc_args_t) && pargs.magic == AB_PROC_ARGS_MAGIC) {
  38 |             for (uint32_t i = 0; i < act_h && i < ULIBC_MAX_STARTUP_HANDLES; i++) {
  39 |                 __ulibc_handle_types[i] = pargs.handle_types[i];
  40 |                 if (pargs.handle_types[i] == AB_PA_LOG && __ulibc_log_h == AB_HANDLE_INVALID) {
  41 |                     __ulibc_log_h = __ulibc_handles[i];
  42 |                 } else if (pargs.handle_types[i] == AB_PA_CONSOLE && __ulibc_con_ch == AB_HANDLE_INVALID) {
  43 |                     __ulibc_con_ch = __ulibc_handles[i];
  44 |                 }
  45 |             }
       //// Backward-compatible fallback for early lessons (prior to `ab_proc_args_t` type
       //// tagging) that sent raw `[log_h, con_ch]` handle arrays with an empty byte
       //// payload.
  46 |         } else {
  47 |             /* Legacy fallback if no ab_proc_args_t header was sent */
  48 |             __ulibc_log_h  = __ulibc_handles[0];
  49 |             __ulibc_con_ch = __ulibc_handles[1];
  50 |         }
  51 |     }
  52 | }
  53 | 
       //// Positional startup-handle accessor (`ulibc_get_startup_handle`): used by
       //// `console-srv`, `fs-srv`, and `shell` to retrieve fixed-position capability
       //// arrays (such as the 4 per-VT server endpoints at indices `3..6`).
  54 | ab_handle_t ulibc_get_startup_handle(uint32_t index) {
  55 |     if (index < __ulibc_num_handles) {
  56 |         return __ulibc_handles[index];
  57 |     }
  58 |     return AB_HANDLE_INVALID;
  59 | }
  60 | 
       //// Type-tagged startup-handle accessor (`ulibc_get_startup_handle_by_type`):
       //// scans `__ulibc_handle_types[]` for a matching `AB_PA_*` constant, decoupling
       //// callers from exact slot numbering just like Zircon's `PA_HND(type, idx)`.
  61 | ab_handle_t ulibc_get_startup_handle_by_type(uint8_t pa_type) {
  62 |     for (uint32_t i = 0; i < __ulibc_num_handles; i++) {
  63 |         if (__ulibc_handle_types[i] == pa_type) {
  64 |             return __ulibc_handles[i];
  65 |         }
  66 |     }
  67 |     return AB_HANDLE_INVALID;
  68 | }
  69 | 
       //// Simple getters exposing `__ulibc_arg1`, `__ulibc_log_h`, and `__ulibc_con_ch`.
  70 | uint64_t ulibc_get_startup_arg1(void) {
  71 |     return __ulibc_arg1;
  72 | }
  73 | 
  74 | ab_handle_t ulibc_get_log_handle(void) {
  75 |     return __ulibc_log_h;
  76 | }
  77 | 
  78 | ab_handle_t ulibc_get_console_handle(void) {
  79 |     return __ulibc_con_ch;
  80 | }
  81 | 
       //// `u_puts(const char *s)`: adaptive standard output router! If the process was
       //// granted an `AB_PA_CONSOLE` channel (such as `calc.bin` or `shell.bin`), text
       //// is sent over IPC to `console-srv` via `shell_print()` so it appears in the
       //// owning Virtual Terminal's screen buffer. Otherwise, if the process only has
       //// `AB_PA_LOG` (such as `fs-srv`), output falls back cleanly to the kernel
       //// debuglog (`ulog`).
  82 | void u_puts(const char *s) {
  83 |     if (__ulibc_con_ch != AB_HANDLE_INVALID) {
  84 |         shell_print(__ulibc_con_ch, s);
  85 |     } else if (__ulibc_log_h != AB_HANDLE_INVALID) {
  86 |         ulog(__ulibc_log_h, s);
  87 |     }
  88 | }
  89 | 
       //// `u_gets(char *out_buf, size_t max_len)`: reads a canonical edited line from
       //// `console-srv` over `__ulibc_con_ch` (`CON_OP_READ_LINE`). If the process has
       //// no console capability, safely returns an empty NUL-terminated string.
  90 | void u_gets(char *out_buf, size_t max_len) {
  91 |     if (__ulibc_con_ch != AB_HANDLE_INVALID) {
  92 |         shell_readline(__ulibc_con_ch, out_buf, max_len);
  93 |     } else if (max_len > 0) {
  94 |         out_buf[0] = '\0';
  95 |     }
  96 | }
  97 | 
       //// `u_dlog(const char *msg)`: writes directly to the kernel `LogDispatcher` if
       //// `AB_PA_LOG` was granted, silently no-oping if logging capability was withheld.
  98 | void u_dlog(const char *msg) {
  99 |     if (__ulibc_log_h != AB_HANDLE_INVALID) {
 100 |         ulog(__ulibc_log_h, msg);
 101 |     }
 102 | }
 103 | 
       //// Standard `printf()` for standalone EL0 programs: formats variadic arguments
       //// into a 256-byte stack buffer via `u_vsnprintf()` and emits the result through
       //// `u_puts()` (routing over IPC to `console-srv`).
 104 | int printf(const char *fmt, ...) {
 105 |     char buf[256];
 106 |     va_list ap;
 107 |     va_start(ap, fmt);
 108 |     int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
 109 |     va_end(ap);
 110 |     u_puts(buf);
 111 |     return n;
 112 | }
 113 | 
       //// `dlog_printf()`: formats variadic arguments into a 256-byte stack buffer and
       //// writes an atomic diagnostic line to the kernel `debuglog` via `u_dlog()`.
 114 | int dlog_printf(const char *fmt, ...) {
 115 |     char buf[256];
 116 |     va_list ap;
 117 |     va_start(ap, fmt);
 118 |     int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
 119 |     va_end(ap);
 120 |     u_dlog(buf);
 121 |     return n;
 122 | }
```

### 13.5.3 Technical Notes

1. **Bootstrap Handle Convention Across ABOS Processes**: The table below summarizes the exact `AB_PA_*` handle arrays unpacked by `__ulibc_init()` across every standalone binary in Lesson 20:

| Standalone Binary | Spawner | `arg1` (`x1`) | Unpacked Startup Handles (`__ulibc_handles[0..N-1]`) |
| :--- | :--- | :---: | :--- |
| `console-srv.bin` | `userboot` | `0` | `[0]=LOG`, `[1]=VMAR_ROOT`, `[2]=RESOURCE`, `[3..6]=CHANNEL_SRV (tty0..3)` |
| `fs-srv.bin` | `userboot` | `0` | `[0]=LOG`, `[1]=VMAR_ROOT`, `[2]=RESOURCE`, `[3..6]=CHANNEL_SRV (tty0..3)` |
| `shell.bin` (`0..3`) | `userboot` | `vt_id` (`0..3`) | `[0]=LOG`, `[1]=VMAR_ROOT`, `[2]=CONSOLE`, `[3]=FS`, `[4]=JOB_DEFAULT` |
| `calc.bin` | `abos-shell-i` | `0` | `[0]=LOG`, `[1]=CONSOLE` *(minimal leaf-app sandbox!)* |

2. **Automatic `printf()` Fallback**: In `u_puts()` (lines 82–88), if `__ulibc_con_ch` is valid (`AB_PA_CONSOLE` was supplied), `printf()` transmits formatted text over IPC to `console-srv`; otherwise, if `__ulibc_log_h` is valid (`AB_PA_LOG`), `printf()` transparently routes output to the kernel `debuglog`. If neither capability was granted to a sandboxed compute process, `u_puts()` safely becomes a no-op rather than faulting.

---

## 13.6 user/ulibc/ulibc.c

### 13.6.1 Functional Description

`user/ulibc/ulibc.c` is the workhorse of ABOS user space (534 lines), organized into three clean subsystems:
1. **`__ab_syscall4` and the 34 System Call Stubs (lines 5–182)**: Executes `svc #0` using the AArch64 register ABI (`x8` = syscall number, `x0..x3` = arguments, `x0` = return status) and marshals multi-argument calls through stack-allocated parameter blocks (`ab_channel_write_args_t`, `ab_vmar_map_args_t`, etc.).
2. **String, Numeric, Monotonic Clock & Console IPC Client Helpers (lines 187–339)**: Implements freestanding `memcpy`/`memset`, `u_*` string helpers, strict `u_atoi`, overflow-safe `u_itoa`, the trapless `cntpct_el0` nanosecond clock `ulibc_clock_get_monotonic()`, chunked request-ACK `shell_print()`, and blocking `shell_readline()`.
3. **Formatted `vsnprintf` & Zircon Size Formatter (lines 344–534)**: Implements `u_vsnprintf()`, `u_snprintf()`, `con_printf()`, and `u_format_size()` (modeled after Zircon's `system/ulib/pretty/sizes.c`) using pure 64-bit integer arithmetic compatible with `-mgeneral-regs-only`.

### 13.6.2 Heavily Commented Source Code

#### Program 13-5: `lesson_20/user/ulibc/ulibc.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/ulibc/ulibc.c" start="1" end="534" total="534" -->
```c
       //// File header block: `ulibc.c` is compiled into `ulibc.o` (archived into
       //// `libulibc.a` for standalone binaries, and also linked directly alongside
       //// `userboot.o` in `kernel.elf` so `userboot` can invoke system calls without
       //// pulling in `crt0.o` or `crt_env.o`'s `.bss` globals).
   1 | /* =========================================================================
   2 |  * user/ulibc/ulibc.c - ABOS Wrapped System Calls & Standard Utility Library
   3 |  * ========================================================================= */
   4 | #include "ulibc.h"
       //// Universal EL0 -> EL1 system call gateway `__ab_syscall4`:
       ////   - Binds C variables directly to hardware registers `x8` (syscall number)
       ////     and `x0..x3` (up to 4 arguments), matching `syscall_dispatch(iframe_t
       ////     *)` in `kernel/core/syscalls.c`.
       ////   - Executes `svc #0`, triggering a Synchronous Exception from Lower EL
       ////     using AArch64 (`ESR_EL1.EC == 0x15`), transitioning the CPU from `EL0t`
       ////     to `EL1h` on the current thread's kernel stack.
       ////   - Marks `x0..x3` with `"+r"` (read-write register operands) and specifies
       ////     the `"memory"` clobber. The `"memory"` clobber is essential: several
       ////     wrappers below pack extra parameters into stack-local structs (such as
       ////     `ab_channel_write_args_t` or `ab_vmar_map_args_t`) and pass `&args` in
       ////     `x1` or `x2`. Without `"memory"`, an optimizing compiler could sink or
       ////     elide stores to `args` before `svc #0` or cache stale values from output
       ////     pointers (`*out_h`, `*actual_bytes`, `*out_pkt`) across the trap!
       ////   - Returns `(ab_status_t)x0` (`0 == AB_OK`, negative `AB_ERR_*` on
       ////     failure).
   5 | ab_status_t __ab_syscall4(uint64_t a0, uint64_t a1, uint64_t a2,
   6 |                           uint64_t a3, uint64_t syscall_num) {
   7 |     register uint64_t x8 __asm__("x8") = syscall_num;
   8 |     register uint64_t x0 __asm__("x0") = a0;
   9 |     register uint64_t x1 __asm__("x1") = a1;
  10 |     register uint64_t x2 __asm__("x2") = a2;
  11 |     register uint64_t x3 __asm__("x3") = a3;
  12 |     __asm__ volatile("svc #0"
  13 |                      : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
  14 |                      : "r"(x8)
  15 |                      : "memory");
  16 |     return (ab_status_t)x0;
  17 | }
  18 | 
       //// Section 1 — Thin typed C wrappers over `__ab_syscall4` for all 34 ABOS system
       //// calls (`SYS_HANDLE_CLOSE = 1` through `SYS_VMAR_UNMAP = 34`).
  19 | /* =========================================================================
  20 |  * 1. Wrapped ABOS System Calls (calling __ab_syscall4 in crt0.S)
  21 |  * ========================================================================= */
       //// `sys_debuglog_write` (`SYS_DEBUGLOG_WRITE = 4`) and `sys_nanosleep`
       //// (`SYS_NANOSLEEP = 6`): fit directly in 4 argument registers (`x0..x3`).
  22 | ab_status_t sys_debuglog_write(ab_handle_t log_h, uint32_t flags, const char *msg, size_t len) {
  23 |     return __ab_syscall4(log_h, flags, (uintptr_t)msg, len, SYS_DEBUGLOG_WRITE);
  24 | }
  25 | 
  26 | ab_status_t sys_nanosleep(uint64_t ticks) {
  27 |     return __ab_syscall4(ticks, 0, 0, 0, SYS_NANOSLEEP);
  28 | }
  29 | 
       //// `sys_process_exit` (`SYS_PROCESS_EXIT = 5`): terminates the calling process
       //// with `retcode`. The trailing `for (;;) { }` satisfies the compiler that
       //// control never falls off the end of this `void` noreturn function.
  30 | void sys_process_exit(int64_t retcode) {
  31 |     (void)__ab_syscall4((uint64_t)retcode, 0, 0, 0, SYS_PROCESS_EXIT);
  32 |     for (;;) { }
  33 | }
  34 | 
       //// Capability handle operations (`SYS_HANDLE_CLOSE = 1`, `SYS_HANDLE_DUPLICATE =
       //// 2`, `SYS_OBJECT_GET_INFO = 3`): pass user-space output pointers (`out_h`,
       //// `out_info`) cast to `uintptr_t`, which the kernel populates safely via
       //// `copy_to_user()`.
  35 | ab_status_t sys_handle_close(ab_handle_t h) {
  36 |     return __ab_syscall4(h, 0, 0, 0, SYS_HANDLE_CLOSE);
  37 | }
  38 | 
  39 | ab_status_t sys_handle_duplicate(ab_handle_t h, ab_rights_t rights, ab_handle_t *out_h) {
  40 |     return __ab_syscall4(h, rights, (uintptr_t)out_h, 0, SYS_HANDLE_DUPLICATE);
  41 | }
  42 | 
  43 | ab_status_t sys_object_get_info(ab_handle_t h, ab_handle_basic_info_t *out_info) {
  44 |     return __ab_syscall4(h, (uintptr_t)out_info, 0, 0, SYS_OBJECT_GET_INFO);
  45 | }
  46 | 
       //// Event, EventPair, and synchronous StateTracker wait wrappers
       //// (`SYS_EVENT_CREATE = 7` .. `SYS_OBJECT_WAIT_ONE = 11`). Notice how
       //// `sys_object_wait_one` packs `(h, signals, deadline, out_observed)` cleanly
       //// into `(x0, x1, x2, x3)`.
  47 | ab_status_t sys_event_create(uint32_t options, ab_handle_t *out_h) {
  48 |     return __ab_syscall4(options, (uintptr_t)out_h, 0, 0, SYS_EVENT_CREATE);
  49 | }
  50 | 
  51 | ab_status_t sys_eventpair_create(uint32_t options, ab_handle_t *out_h0, ab_handle_t *out_h1) {
  52 |     return __ab_syscall4(options, (uintptr_t)out_h0, (uintptr_t)out_h1, 0, SYS_EVENTPAIR_CREATE);
  53 | }
  54 | 
  55 | ab_status_t sys_object_signal(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask) {
  56 |     return __ab_syscall4(h, clear_mask, set_mask, 0, SYS_OBJECT_SIGNAL);
  57 | }
  58 | 
  59 | ab_status_t sys_object_signal_peer(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask) {
  60 |     return __ab_syscall4(h, clear_mask, set_mask, 0, SYS_OBJECT_SIGNAL_PEER);
  61 | }
  62 | 
  63 | ab_status_t sys_object_wait_one(ab_handle_t h, ab_signals_t signals,
  64 |                                 uint64_t deadline, ab_signals_t *out_observed) {
  65 |     return __ab_syscall4(h, signals, deadline, (uintptr_t)out_observed, SYS_OBJECT_WAIT_ONE);
  66 | }
  67 | 
       //// Asynchronous Port multiplexing wrappers (`SYS_PORT_CREATE = 12` ..
       //// `SYS_OBJECT_WAIT_ASYNC = 15`). `sys_object_wait_async` registers a one-shot
       //// `WaitStateObserver` on `obj_h` that enqueues an `AB_PKT_TYPE_SIGNAL_ONE`
       //// packet tagged with `key` onto `port_h` as soon as any bit in `signals` becomes
       //// asserted.
  68 | ab_status_t sys_port_create(uint32_t options, ab_handle_t *out_port) {
  69 |     return __ab_syscall4(options, (uintptr_t)out_port, 0, 0, SYS_PORT_CREATE);
  70 | }
  71 | 
  72 | ab_status_t sys_port_queue(ab_handle_t port_h, const ab_port_packet_t *pkt) {
  73 |     return __ab_syscall4(port_h, (uintptr_t)pkt, 0, 0, SYS_PORT_QUEUE);
  74 | }
  75 | 
  76 | ab_status_t sys_port_wait(ab_handle_t port_h, uint64_t deadline, ab_port_packet_t *out_pkt) {
  77 |     return __ab_syscall4(port_h, deadline, (uintptr_t)out_pkt, 0, SYS_PORT_WAIT);
  78 | }
  79 | 
  80 | ab_status_t sys_object_wait_async(ab_handle_t obj_h, ab_handle_t port_h,
  81 |                                   uint64_t key, ab_signals_t signals) {
  82 |     return __ab_syscall4(obj_h, port_h, key, signals, SYS_OBJECT_WAIT_ASYNC);
  83 | }
  84 | 
       //// `sys_channel_create` (`SYS_CHANNEL_CREATE = 16`): creates two entangled peer
       //// `ChannelDispatcher` objects and writes their handles to `*out_ch0` and
       //// `*out_ch1`.
  85 | ab_status_t sys_channel_create(uint32_t options, ab_handle_t *out_ch0, ab_handle_t *out_ch1) {
  86 |     return __ab_syscall4(options, (uintptr_t)out_ch0, (uintptr_t)out_ch1, 0, SYS_CHANNEL_CREATE);
  87 | }
  88 | 
       //// `sys_channel_write` (`SYS_CHANNEL_WRITE = 17`) and `sys_channel_read`
       //// (`SYS_CHANNEL_READ = 18`): because channel operations require 5 to 7
       //// parameters (channel handle, options, byte buffer + length, handle array +
       //// count, plus actual counts on read) while `__ab_syscall4` passes 4 registers,
       //// these wrappers marshal the buffer/handle descriptors into a stack-allocated
       //// `ab_channel_write_args_t` / `ab_channel_read_args_t` block and pass `&args` in
       //// `x2` (`a1 = 0` reserved for Zircon-style `options`).
  89 | ab_status_t sys_channel_write(ab_handle_t ch_h, const void *bytes, uint32_t num_bytes,
  90 |                               const ab_handle_t *handles, uint32_t num_handles) {
  91 |     ab_channel_write_args_t args = { bytes, num_bytes, handles, num_handles };
  92 |     return __ab_syscall4(ch_h, 0, (uintptr_t)&args, 0, SYS_CHANNEL_WRITE);
  93 | }
  94 | 
  95 | ab_status_t sys_channel_read(ab_handle_t ch_h, void *bytes, uint32_t buf_bytes,
  96 |                              ab_handle_t *handles, uint32_t buf_handles,
  97 |                              uint32_t *actual_bytes, uint32_t *actual_handles) {
  98 |     ab_channel_read_args_t args = {
  99 |         bytes, handles, buf_bytes, buf_handles, actual_bytes, actual_handles
 100 |     };
 101 |     return __ab_syscall4(ch_h, 0, (uintptr_t)&args, 0, SYS_CHANNEL_READ);
 102 | }
 103 | 
       //// VMO creation and direct kernel-mediated byte I/O (`SYS_VMO_CREATE = 19`,
       //// `SYS_VMO_READ = 20`, `SYS_VMO_WRITE = 21`).
 104 | ab_status_t sys_vmo_create(uint64_t size, uint32_t options, ab_handle_t *out_vmo) {
 105 |     return __ab_syscall4(size, options, (uintptr_t)out_vmo, 0, SYS_VMO_CREATE);
 106 | }
 107 | 
 108 | ab_status_t sys_vmo_read(ab_handle_t vmo_h, void *buf, uint64_t offset, size_t len) {
 109 |     return __ab_syscall4(vmo_h, (uintptr_t)buf, offset, len, SYS_VMO_READ);
 110 | }
 111 | 
 112 | ab_status_t sys_vmo_write(ab_handle_t vmo_h, const void *buf, uint64_t offset, size_t len) {
 113 |     return __ab_syscall4(vmo_h, (uintptr_t)buf, offset, len, SYS_VMO_WRITE);
 114 | }
 115 | 
       //// `sys_vmar_map` (`SYS_VMAR_MAP = 22`) and `sys_vmar_unmap` (`SYS_VMAR_UNMAP =
       //// 34`): `sys_vmar_map` packs `(vmo_h, vmo_offset, len, perms, out_va)` into
       //// `ab_vmar_map_args_t` on the stack, allowing 7 logical arguments to cross `svc
       //// #0` in 3 registers (`vmar_h`, `vmar_offset`, `&args`).
 116 | ab_status_t sys_vmar_map(ab_handle_t vmar_h, uint64_t vmar_offset,
 117 |                          ab_handle_t vmo_h, uint64_t vmo_offset,
 118 |                          uint64_t len, uint32_t perms, vaddr_t *out_va) {
 119 |     ab_vmar_map_args_t args = { vmo_h, vmo_offset, len, perms, out_va };
 120 |     return __ab_syscall4(vmar_h, vmar_offset, (uintptr_t)&args, 0, SYS_VMAR_MAP);
 121 | }
 122 | 
 123 | ab_status_t sys_vmar_unmap(ab_handle_t vmar_h, vaddr_t addr, uint64_t len) {
 124 |     return __ab_syscall4(vmar_h, addr, len, 0, SYS_VMAR_UNMAP);
 125 | }
 126 | 
       //// Task creation & startup wrappers (`SYS_PROCESS_CREATE = 23`,
       //// `SYS_THREAD_CREATE = 24`, `SYS_PROCESS_START = 25`): notice that
       //// `sys_process_create` and `sys_thread_create` compute `u_strlen(name)` in user
       //// space so the kernel's `copy_from_user()` knows the exact bounded string length
       //// without scanning untrusted user memory byte-by-byte.
 127 | ab_status_t sys_process_create(ab_handle_t job_h, const char *name,
 128 |                                ab_handle_t *out_proc, ab_handle_t *out_vmar) {
 129 |     ab_process_create_args_t args = { name, u_strlen(name), out_proc, out_vmar };
 130 |     return __ab_syscall4(job_h, (uintptr_t)&args, 0, 0, SYS_PROCESS_CREATE);
 131 | }
 132 | 
 133 | ab_status_t sys_thread_create(ab_handle_t proc_h, const char *name, ab_handle_t *out_thr) {
 134 |     return __ab_syscall4(proc_h, (uintptr_t)name, u_strlen(name), (uintptr_t)out_thr, SYS_THREAD_CREATE);
 135 | }
 136 | 
 137 | ab_status_t sys_process_start(ab_handle_t proc_h, ab_handle_t thr_h, uintptr_t entry,
 138 |                               uintptr_t sp, ab_handle_t arg_h, uintptr_t arg2) {
 139 |     ab_process_start_args_t args = { thr_h, entry, sp, arg_h, arg2 };
 140 |     return __ab_syscall4(proc_h, (uintptr_t)&args, 0, 0, SYS_PROCESS_START);
 141 | }
 142 | 
       //// Driver capability wrappers (`SYS_VMO_CREATE_PHYSICAL = 26`,
       //// `SYS_VMO_CREATE_CONTIG = 27`, `SYS_INTERRUPT_CREATE = 28`, `SYS_INTERRUPT_WAIT
       //// = 29`, `SYS_INTERRUPT_ACK = 30`): every creation call presents `res_h`
       //// (`AB_OBJ_TYPE_RESOURCE`) in `x0` as proof of hardware authority.
 143 | ab_status_t sys_vmo_create_physical(ab_handle_t res_h, paddr_t pa, uint64_t size,
 144 |                                     ab_handle_t *out_vmo) {
 145 |     ab_vmo_create_phys_args_t args = { pa, size, out_vmo };
 146 |     return __ab_syscall4(res_h, (uintptr_t)&args, 0, 0, SYS_VMO_CREATE_PHYSICAL);
 147 | }
 148 | 
 149 | ab_status_t sys_vmo_create_contiguous(ab_handle_t res_h, uint64_t size,
 150 |                                       ab_handle_t *out_vmo, paddr_t *out_pa) {
 151 |     ab_vmo_create_contig_args_t args = { size, out_vmo, out_pa };
 152 |     return __ab_syscall4(res_h, (uintptr_t)&args, 0, 0, SYS_VMO_CREATE_CONTIG);
 153 | }
 154 | 
 155 | ab_status_t sys_interrupt_create(ab_handle_t res_h, uint32_t vector, ab_handle_t *out_irq) {
 156 |     return __ab_syscall4(res_h, vector, (uintptr_t)out_irq, 0, SYS_INTERRUPT_CREATE);
 157 | }
 158 | 
 159 | ab_status_t sys_interrupt_wait(ab_handle_t irq_h) {
 160 |     return __ab_syscall4(irq_h, 0, 0, 0, SYS_INTERRUPT_WAIT);
 161 | }
 162 | 
 163 | ab_status_t sys_interrupt_ack(ab_handle_t irq_h) {
 164 |     return __ab_syscall4(irq_h, 0, 0, 0, SYS_INTERRUPT_ACK);
 165 | }
 166 | 
       //// Object naming (`SYS_OBJECT_SET_NAME = 31`) and Zircon-style task/VM
       //// introspection (`SYS_JOB_GET_PS_INFO = 32`, `SYS_PROCESS_GET_MAPS = 33`),
       //// powering the interactive `ps` and `vmaps` shell utilities.
 167 | ab_status_t sys_object_set_name(ab_handle_t h, const char *name) {
 168 |     return __ab_syscall4(h, (uintptr_t)name, name ? u_strlen(name) : 0, 0, SYS_OBJECT_SET_NAME);
 169 | }
 170 | 
 171 | ab_status_t sys_job_get_ps_info(ab_handle_t job_h, ab_task_info_entry_t *entries,
 172 |                                 uint32_t max_entries, uint32_t *actual_out) {
 173 |     return __ab_syscall4(job_h, (uintptr_t)entries, max_entries, (uintptr_t)actual_out,
 174 |                          SYS_JOB_GET_PS_INFO);
 175 | }
 176 | 
 177 | ab_status_t sys_process_get_maps(ab_handle_t job_h, ab_koid_t proc_koid,
 178 |                                  ab_info_maps_t *maps, uint32_t max_maps,
 179 |                                  uint32_t *actual_out) {
 180 |     ab_process_get_maps_args_t args = { proc_koid, maps, max_maps, actual_out };
 181 |     return __ab_syscall4(job_h, (uintptr_t)&args, 0, 0, SYS_PROCESS_GET_MAPS);
 182 | }
 183 | 
       //// Section 2 — Freestanding memory, string, numeric, clock, and IPC helpers.
       //// Providing `memcpy` and `memset` with exact standard C signatures is mandatory
       //// even under `-ffreestanding -nostdlib`: GCC automatically emits calls to
       //// `memcpy` and `memset` whenever C code initializes stack structs (such as
       //// `console_msg_t req = { CON_OP_READ_LINE, "" };` or `ab_channel_write_args_t
       //// args = { ... };`).
 184 | /* =========================================================================
 185 |  * 2. Standard String, Number & IPC Console Helpers
 186 |  * ========================================================================= */
 187 | void *memcpy(void *dst, const void *src, size_t n) {
 188 |     uint8_t *d = (uint8_t *)dst;
 189 |     const uint8_t *sr = (const uint8_t *)src;
 190 |     for (size_t i = 0; i < n; i++) d[i] = sr[i];
 191 |     return dst;
 192 | }
 193 | 
 194 | void *memset(void *dst, int c, size_t n) {
 195 |     uint8_t *d = (uint8_t *)dst;
 196 |     for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
 197 |     return dst;
 198 | }
 199 | 
       //// Compact NUL-terminated string primitives (`u_strlen`, `u_strcmp`, `u_strncmp`,
       //// `u_strcpy`), prefixed with `u_` so they never clash with compiler built-in
       //// declarations or kernel symbols when `ulibc.o` is linked into `kernel.elf` for
       //// `userboot`.
 200 | size_t u_strlen(const char *s) {
 201 |     size_t n = 0;
 202 |     while (s[n]) n++;
 203 |     return n;
 204 | }
 205 | 
 206 | int u_strcmp(const char *a, const char *b) {
 207 |     while (*a && (*a == *b)) { a++; b++; }
 208 |     return (unsigned char)*a - (unsigned char)*b;
 209 | }
 210 | 
 211 | int u_strncmp(const char *a, const char *b, size_t n) {
 212 |     for (size_t i = 0; i < n; i++) {
 213 |         if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
 214 |             return (unsigned char)a[i] - (unsigned char)b[i];
 215 |         }
 216 |     }
 217 |     return 0;
 218 | }
 219 | 
 220 | void u_strcpy(char *dst, const char *src) {
 221 |     while ((*dst++ = *src++) != '\0') { }
 222 | }
 223 | 
       //// `u_atoi`: strict signed 64-bit decimal parser used by `shell.c` (`vmaps
       //// <koid>`) and `calc.c`. Unlike classic C `atoi()`, which silently returns `0`
       //// on `"abc"` or `"12xyz"`, `u_atoi` strips leading/trailing whitespace, requires
       //// at least one digit, rejects any trailing non-digit garbage, and reports
       //// validity explicitly via `*out_ok`.
 224 | int64_t u_atoi(const char *s, int *out_ok) {
 225 |     while (*s == ' ' || *s == '\t') s++;
 226 |     int sign = 1;
 227 |     if (*s == '-') { sign = -1; s++; }
 228 |     else if (*s == '+') { s++; }
 229 |     if (*s < '0' || *s > '9') {
 230 |         if (out_ok) *out_ok = 0;
 231 |         return 0;
 232 |     }
 233 |     int64_t val = 0;
 234 |     while (*s >= '0' && *s <= '9') {
 235 |         val = val * 10 + (*s - '0');
 236 |         s++;
 237 |     }
 238 |     while (*s == ' ' || *s == '\t') s++;
 239 |     if (*s != '\0') {
 240 |         if (out_ok) *out_ok = 0;
 241 |         return 0;
 242 |     }
 243 |     if (out_ok) *out_ok = 1;
 244 |     return sign * val;
 245 | }
 246 | 
       //// `ulibc_clock_get_monotonic`: zero-syscall user-space monotonic clock
       //// (analogous to Zircon's VDSO `zx_clock_get_monotonic`)!
       ////   1. During boot, `timer_init_percpu()` in `kernel/dev/timer.c` writes
       ////      `CNTKCTL_EL1 = 1` (`EL0PCTEN`), permitting `EL0` to read the ARMv8
       ////      physical counter (`CNTPCT_EL0`) and frequency (`CNTFRQ_EL0`, 62.5 MHz
       ////      on QEMU `virt`) directly without trapping to `EL1`.
       ////   2. The `isb` barrier before `mrs %0, cntpct_el0` prevents the CPU pipeline
       ////      from speculatively reading the counter ahead of prior loads/branches.
       ////   3. Splitting `cnt` into `sec = cnt / freq` and `rem = cnt % freq` before
       ////      multiplying by `1,000,000,000ULL` avoids 64-bit unsigned overflow (`rem
       ////      < freq`, so `rem * 10^9` easily fits in `uint64_t` for any GHz-class
       ////      timer frequency).
 247 | ab_time_t ulibc_clock_get_monotonic(void) {
 248 |     uint64_t cnt = 0, freq = 0;
 249 |     __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(cnt));
 250 |     __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
 251 |     if (freq == 0) return 0;
 252 |     uint64_t sec = cnt / freq;
 253 |     uint64_t rem = cnt % freq;
 254 |     return sec * 1000000000ULL + (rem * 1000000000ULL) / freq;
 255 | }
 256 | 
       //// `u_itoa`: converts a signed 64-bit integer to ASCII decimal. Note line 267:
       //// `uval = 0ULL - (uint64_t)val;` computes the magnitude in unsigned two's
       //// complement arithmetic, avoiding signed overflow undefined behavior when `val
       //// == INT64_MIN` (`-9223372036854775808`).
 257 | void u_itoa(int64_t val, char *buf) {
 258 |     if (val == 0) {
 259 |         buf[0] = '0';
 260 |         buf[1] = '\0';
 261 |         return;
 262 |     }
 263 |     int neg = 0;
 264 |     uint64_t uval;
 265 |     if (val < 0) {
 266 |         neg = 1;
 267 |         uval = 0ULL - (uint64_t)val;
 268 |     } else {
 269 |         uval = (uint64_t)val;
 270 |     }
 271 |     char tmp[32];
 272 |     int i = 0;
 273 |     while (uval > 0) {
 274 |         tmp[i++] = (char)('0' + (uval % 10));
 275 |         uval /= 10;
 276 |     }
 277 |     int pos = 0;
 278 |     if (neg) buf[pos++] = '-';
 279 |     while (i > 0) {
 280 |         buf[pos++] = tmp[--i];
 281 |     }
 282 |     buf[pos] = '\0';
 283 | }
 284 | 
       //// `ulog`: convenience wrapper writing a NUL-terminated string to a kernel
       //// `LogDispatcher` handle.
 285 | void ulog(ab_handle_t log_h, const char *msg) {
 286 |     sys_debuglog_write(log_h, 0, msg, u_strlen(msg));
 287 | }
 288 | 
       //// `shell_print`: synchronous chunked IPC writer to `console-srv`.
       ////   - `console_msg_t` carries a 4-byte `op` (`CON_OP_WRITE`) plus `char
       ////     text[240]` (244 bytes total, fitting comfortably inside
       ////     `CHANNEL_MAX_MSG_BYTES = 256`).
       ////   - Strings longer than 239 bytes (such as `help` banners or `ps`/`vmaps`
       ////     rows) are sliced into 239-byte NUL-terminated chunks.
       ////   - After sending each chunk (`sys_channel_write`), `shell_print` waits for
       ////     `console-srv` to reply with a 4-byte `CON_OP_WRITE` acknowledgment
       ////     before sending the next chunk. This synchronous request/ACK flow control
       ////     guarantees ordered output and prevents a fast producer (`cat` or `vmaps
       ////     all`) from flooding the channel queue ahead of UART transmission.
 289 | void shell_print(ab_handle_t con_ch, const char *text) {
 290 |     console_msg_t msg;
 291 |     msg.op = CON_OP_WRITE;
 292 |     size_t i = 0;
 293 |     while (text[i]) {
 294 |         size_t chunk = 0;
 295 |         while (text[i] && chunk + 1 < sizeof(msg.text)) {
 296 |             msg.text[chunk++] = text[i++];
 297 |         }
 298 |         msg.text[chunk] = '\0';
 299 |         if (sys_channel_write(con_ch, &msg, sizeof(msg), NULL, 0) != AB_OK) {
 300 |             return;
 301 |         }
 302 | 
 303 |         ab_signals_t obs = 0;
 304 |         sys_object_wait_one(con_ch, AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED,
 305 |                             AB_TIME_INFINITE, &obs);
 306 |         if (!(obs & AB_SIGNAL_READABLE)) {
 307 |             return;
 308 |         }
 309 |         uint32_t ack = 0, ab = 0, ah = 0;
 310 |         sys_channel_read(con_ch, &ack, sizeof(ack), NULL, 0, &ab, &ah);
 311 |     }
 312 | }
 313 | 
       //// `shell_readline`: synchronous RPC client for line-buffered terminal input.
       //// Sends a 4-byte `CON_OP_READ_LINE` request on `con_ch`, sleeps in the kernel's
       //// `StateTracker` wait queue (`sys_object_wait_one`) consuming zero CPU until
       //// either `console-srv` delivers a completed line (`AB_SIGNAL_READABLE`) or
       //// closes the peer endpoint (`AB_SIGNAL_PEER_CLOSED`, e.g., after another VT
       //// executes `exit`), in which case `shell_readline` cleanly exits the client
       //// process via `sys_process_exit(0)`.
 314 | void shell_readline(ab_handle_t con_ch, char *out_buf, size_t max_len) {
 315 |     console_msg_t req = { CON_OP_READ_LINE, "" };
 316 |     if (sys_channel_write(con_ch, &req, sizeof(uint32_t), NULL, 0) != AB_OK) {
 317 |         sys_process_exit(0);
 318 |     }
 319 | 
 320 |     ab_signals_t obs = 0;
 321 |     sys_object_wait_one(con_ch, AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED,
 322 |                         AB_TIME_INFINITE, &obs);
 323 |     if (!(obs & AB_SIGNAL_READABLE)) {
 324 |         sys_process_exit(0);
 325 |     }
 326 | 
 327 |     console_msg_t rep;
 328 |     uint32_t ab = 0, ah = 0;
 329 |     if (sys_channel_read(con_ch, &rep, sizeof(rep), NULL, 0, &ab, &ah) != AB_OK) {
 330 |         sys_process_exit(0);
 331 |     }
 332 | 
 333 |     size_t i = 0;
 334 |     while (rep.text[i] && i + 1 < max_len) {
 335 |         out_buf[i] = rep.text[i];
 336 |         i++;
 337 |     }
 338 |     out_buf[i] = '\0';
 339 | }
 340 | 
       //// Section 3 — Self-contained freestanding `vsnprintf` formatting engine.
       //// `buf_putc` implements C99 bounded-buffer semantics: characters are written
       //// only while `*pos + 1 < size` (reserving 1 byte for the trailing `'\0'`), while
       //// `*pos` always increments so `u_vsnprintf` returns the total formatted length
       //// that *would* have been written.
 341 | /* =========================================================================
 342 |  * 3. Formatted Output Engine: u_vsnprintf, u_snprintf, con_printf
 343 |  * ========================================================================= */
 344 | static void buf_putc(char *buf, size_t size, size_t *pos, char c) {
 345 |     if (*pos + 1 < size) {
 346 |         buf[*pos] = c;
 347 |     }
 348 |     (*pos)++;
 349 | }
 350 | 
 351 | static void buf_puts(char *buf, size_t size, size_t *pos, const char *s) {
 352 |     while (*s) {
 353 |         buf_putc(buf, size, pos, *s++);
 354 |     }
 355 | }
 356 | 
       //// `buf_put_uint`: converts an unsigned 64-bit integer into base 10 or base 16
       //// (lowercase or uppercase) in reverse digit order inside `tmp[24]`, pads up to
       //// `width` with `pad` (`' '` or `'0'`), and emits characters most-significant
       //// digit first.
 357 | static void buf_put_uint(char *buf, size_t size, size_t *pos,
 358 |                          uint64_t val, uint32_t base, int width, char pad, int upper) {
 359 |     char tmp[24];
 360 |     const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
 361 |     int i = 0;
 362 | 
 363 |     if (val == 0) {
 364 |         tmp[i++] = '0';
 365 |     } else {
 366 |         while (val > 0) {
 367 |             tmp[i++] = digits[val % base];
 368 |             val /= base;
 369 |         }
 370 |     }
 371 |     while (i < width) {
 372 |         tmp[i++] = pad;
 373 |     }
 374 |     while (i > 0) {
 375 |         buf_putc(buf, size, pos, tmp[--i]);
 376 |     }
 377 | }
 378 | 
       //// `u_vsnprintf`: parses format flags (`-` left-align, `0` zero-pad), decimal
       //// field width (e.g., `%-14s`, `%7s`, `%016lx`), and length modifiers (`l` / `ll`
       //// promoting integer arguments to 64-bit `int64_t` / `uint64_t`). Note that
       //// because ABOS compiles with `-mgeneral-regs-only`, floating-point specifiers
       //// (`%f`, `%g`) are intentionally omitted so user binaries never touch NEON/FP
       //// registers.
 379 | int u_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
 380 |     size_t pos = 0;
 381 |     if (!buf || size == 0) return 0;
 382 | 
 383 |     while (*fmt) {
 384 |         if (*fmt != '%') {
 385 |             buf_putc(buf, size, &pos, *fmt++);
 386 |             continue;
 387 |         }
 388 |         fmt++; /* skip '%' */
 389 | 
 390 |         int left_align = 0;
 391 |         if (*fmt == '-') {
 392 |             left_align = 1;
 393 |             fmt++;
 394 |         }
 395 | 
 396 |         char pad = ' ';
 397 |         if (*fmt == '0' && !left_align) {
 398 |             pad = '0';
 399 |             fmt++;
 400 |         }
 401 | 
 402 |         int width = 0;
 403 |         while (*fmt >= '0' && *fmt <= '9') {
 404 |             width = width * 10 + (*fmt - '0');
 405 |             fmt++;
 406 |         }
 407 | 
 408 |         int is_long = 0;
 409 |         while (*fmt == 'l') {
 410 |             is_long = 1;
 411 |             fmt++;
 412 |         }
 413 | 
       //// Format specifier dispatch: `%s` (with `(null)` safety and left/right space
       //// padding for aligned `ps`/`vmaps` columns), `%c`, `%d`/`%i` (using
       //// overflow-safe `0ULL - (uint64_t)v`), `%u`, `%x`/`%X`, `%p` (`0x` followed by
       //// 16 zero-padded hex digits), and `%%`.
 414 |         switch (*fmt) {
 415 |             case 's': {
 416 |                 const char *s = va_arg(ap, const char *);
 417 |                 if (!s) s = "(null)";
 418 |                 int slen = (int)u_strlen(s);
 419 |                 if (!left_align && width > slen) {
 420 |                     for (int p = 0; p < width - slen; p++) buf_putc(buf, size, &pos, ' ');
 421 |                 }
 422 |                 buf_puts(buf, size, &pos, s);
 423 |                 if (left_align && width > slen) {
 424 |                     for (int p = 0; p < width - slen; p++) buf_putc(buf, size, &pos, ' ');
 425 |                 }
 426 |                 break;
 427 |             }
 428 |             case 'c': {
 429 |                 char c = (char)va_arg(ap, int);
 430 |                 buf_putc(buf, size, &pos, c);
 431 |                 break;
 432 |             }
 433 |             case 'd':
 434 |             case 'i': {
 435 |                 int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
 436 |                 if (v < 0) {
 437 |                     buf_putc(buf, size, &pos, '-');
 438 |                     buf_put_uint(buf, size, &pos, 0ULL - (uint64_t)v, 10, width, pad, 0);
 439 |                 } else {
 440 |                     buf_put_uint(buf, size, &pos, (uint64_t)v, 10, width, pad, 0);
 441 |                 }
 442 |                 break;
 443 |             }
 444 |             case 'u': {
 445 |                 uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
 446 |                 buf_put_uint(buf, size, &pos, v, 10, width, pad, 0);
 447 |                 break;
 448 |             }
 449 |             case 'x':
 450 |             case 'X': {
 451 |                 uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
 452 |                 buf_put_uint(buf, size, &pos, v, 16, width, pad, (*fmt == 'X'));
 453 |                 break;
 454 |             }
 455 |             case 'p': {
 456 |                 uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
 457 |                 buf_puts(buf, size, &pos, "0x");
 458 |                 buf_put_uint(buf, size, &pos, v, 16, 16, '0', 0);
 459 |                 break;
 460 |             }
 461 |             case '%':
 462 |                 buf_putc(buf, size, &pos, '%');
 463 |                 break;
 464 |             case '\0':
 465 |                 goto done;
 466 |             default:
 467 |                 buf_putc(buf, size, &pos, '%');
 468 |                 buf_putc(buf, size, &pos, *fmt);
 469 |                 break;
 470 |         }
 471 |         fmt++;
 472 |     }
 473 | 
       //// Always NUL-terminate `buf` at `min(pos, size - 1)` before returning
       //// `(int)pos`.
 474 | done:
 475 |     buf[(pos < size) ? pos : (size - 1)] = '\0';
 476 |     return (int)pos;
 477 | }
 478 | 
       //// `u_snprintf` and `con_printf`: variadic wrappers around `u_vsnprintf`.
       //// `con_printf` formats up to 255 characters on the stack and transmits them over
       //// an explicit `con_ch` capability handle via `shell_print()`.
 479 | int u_snprintf(char *buf, size_t size, const char *fmt, ...) {
 480 |     va_list ap;
 481 |     va_start(ap, fmt);
 482 |     int n = u_vsnprintf(buf, size, fmt, ap);
 483 |     va_end(ap);
 484 |     return n;
 485 | }
 486 | 
 487 | int con_printf(ab_handle_t con_ch, const char *fmt, ...) {
 488 |     char buf[256];
 489 |     va_list ap;
 490 |     va_start(ap, fmt);
 491 |     int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
 492 |     va_end(ap);
 493 |     shell_print(con_ch, buf);
 494 |     return n;
 495 | }
 496 | 
       //// `u_format_size`: faithful freestanding C port of Zircon's
       //// `format_size_fixed()` (`zircon_org/system/ulib/pretty/sizes.c`), used by `ps`
       //// and `vmaps` to render memory sizes compactly (`"24k"`, `"128k"`, `"768M"`,
       //// `"2G"`, `"11.5k"`):
       ////   - Scales down by 1024 (`BkMGTPE`) whenever `bytes >= 10000` OR `bytes` is
       ////     an exact nonzero multiple of 1024 (`(bytes & 1023) == 0`), so `4096`
       ////     renders cleanly as `"4k"` while `1500` stays `"1500B"`.
       ////   - Computes one rounded decimal digit (`r`) using pure integer arithmetic
       ////     (no hardware floating point!), handling carry-over (`r == 10`) into the
       ////     whole-unit count.
 497 | /* Models format_size_fixed() in zircon_org/system/ulib/pretty/sizes.c */
 498 | char *u_format_size(char *str, size_t str_size, size_t bytes) {
 499 |     const char *units = "BkMGTPE";
 500 |     int ui = 0;
 501 |     uint32_t r = 0;
 502 |     int whole = 1;
 503 | 
 504 |     while (bytes >= 10000 || (bytes != 0 && (bytes & 1023) == 0)) {
 505 |         ui++;
 506 |         if (ui >= 6) break;
 507 |         if (bytes & 1023) {
 508 |             whole = 0;
 509 |         }
 510 |         r = (uint32_t)(bytes % 1024);
 511 |         bytes /= 1024;
 512 |     }
 513 | 
 514 |     if (!whole) {
 515 |         uint32_t round_up = ((r % 100) >= 50) ? 1U : 0U;
 516 |         r = (r / 100) + round_up;
 517 |         if (r == 10) {
 518 |             bytes++;
 519 |             r = 0;
 520 |             if (bytes >= 1024 && (bytes & 1023) == 0 && ui < 6) {
 521 |                 bytes /= 1024;
 522 |                 ui++;
 523 |                 whole = 1;
 524 |             }
 525 |         }
 526 |     }
 527 | 
 528 |     if (whole) {
 529 |         u_snprintf(str, str_size, "%lu%c", (uint64_t)bytes, units[ui]);
 530 |     } else {
 531 |         u_snprintf(str, str_size, "%lu.%u%c", (uint64_t)bytes, r, units[ui]);
 532 |     }
 533 |     return str;
 534 | }
       //// End of `user/ulibc/ulibc.c`: in just 534 lines, `ulibc` provides the complete
       //// system call, startup, clock, IPC, and formatting foundation for every EL0
       //// driver, shell, and application in ABOS.
```

### 13.6.3 Technical Notes

1. **Why Multi-Argument Syscalls Use Stack Argument Structs**: `__ab_syscall4` passes 4 64-bit values (`a0..a3`) in `x0..x3` plus `syscall_num` in `x8`. Operations like `sys_channel_read` (7 parameters) and `sys_vmar_map` (7 parameters) pack their remaining parameters into a compact struct (`ab_channel_read_args_t`, `ab_vmar_map_args_t`) on the caller's 24 KB `EL0` stack and pass its user virtual address to `EL1`, where `syscall_dispatch` copies the struct safely via `copy_from_user()`. Because `__ab_syscall4` declares a `"memory"` clobber on the `svc #0` inline assembly, the compiler is guaranteed to materialize every field of `args` into RAM *before* trapping to `EL1`.
2. **VDSO-Style Syscall-Free Timekeeping (`ulibc_clock_get_monotonic`)**: In `console-srv`, every `ESC` (`0x1B`) byte received over UART computes a 25 ms disambiguation deadline (`ulibc_clock_get_monotonic() + ESC_TIMEOUT_NS`). Because `timer_init_percpu()` sets bit 0 (`EL0PCTEN`) of `CNTKCTL_EL1` on all 4 Cortex-A53 cores, `ulibc_clock_get_monotonic()` reads `cntpct_el0` and `cntfrq_el0` in ~2 nanoseconds purely in `EL0` without paying the cost of an `EL0 -> EL1 -> EL0` exception round-trip.
3. **End-to-End Flow Control in `shell_print()`**: Notice that `shell_print()` is *not* fire-and-forget: after writing each 239-byte `CON_OP_WRITE` message, the client blocks in `sys_object_wait_one` until `console-srv` drains the message to the virtual screen buffer (and physical PL011 UART if active) and replies with a 4-byte `ACK`. Without this backpressure, a loop printing hundreds of lines (such as `vmaps all` or `cat` on a large file) could allocate unbounded `ab_message_packet_t` nodes in the kernel heap.
4. **FPU-Free Fixed-Point Formatting (`u_format_size`)**: Because `-mgeneral-regs-only` forbids floating-point instructions in both `EL1` and `EL0` (avoiding saving/restoring 512 bytes of `Q0..Q31` NEON registers on every context switch), `u_format_size` computes rounded tenths of binary units (`k`, `M`, `G`, `T`, `P`, `E`) purely with integer division and remainder rounding (`((r % 100) >= 50) ? 1U : 0U`), matching Zircon's `system/ulib/pretty/sizes.c`.

---

## 13.7 Summary

In this chapter we examined the complete user-space runtime foundation (`user/link.ld` and `user/ulibc/`) shared across ABOS's `EL0` ecosystem:

1. **Unified Flat-Binary Linking (`user/link.ld`)**: Every standalone `EL0` server, shell, and app links at `0x20000000` (`offset 0x10000000` inside `vmar-dynamic`), with `KEEP(*(.text._start))` placing `_start` at byte `0` and `.app_end` ensuring `.bss` is fully covered in raw `.bin` images.
2. **Deterministic C Startup (`crt0.S` & `crt_env.c`)**: `_start` zeroes `.bss` and invokes `__ulibc_init(boot_ch, arg1)`, which unpacks the parent's `ab_proc_args_t` message and `AB_PA_*` capability handles, closes the bootstrap channel immediately, and binds `printf()`, `u_gets()`, and `dlog_printf()` to their inherited capabilities.
3. **Capability Syscalls, Fast Monotonic Time & IPC Helpers (`ulibc.h` & `ulibc.c`)**: `__ab_syscall4` bridges all 34 ABOS system calls across `svc #0`; `ulibc_clock_get_monotonic()` reads `cntpct_el0` directly in `EL0`; and `shell_print()`, `shell_readline()`, `u_vsnprintf()`, and `u_format_size()` turn raw `Channel` capabilities into ergonomic terminal and diagnostic APIs.

Armed with `ulibc` and `user/link.ld`, we are ready in **Chapter 14** to trace how `userboot` bootstraps the user-space capability graph from `bootfs.tar` and how `console-srv` and `fs-srv` implement complete interrupt-driven UART/VT and DMA-driven VirtIO-Blk/TAR device drivers entirely in unprivileged `EL0`.
