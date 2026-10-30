# 16 Building and Packaging the Operating System

In Dr. Zhao Jiong's *A Heavily Commented Linux Kernel Source Code*, Chapter 16 examines how Linux 0.12 uses a hierarchy of `Makefile` rules together with a small host C utility (`tools/build.c`) to concatenate three separately compiled components—`bootsect`, `setup`, and the monolithic `system` kernel—into a single raw floppy-disk Boot `Image`.

An operating system's build and packaging pipeline is a direct mirror of its runtime architecture. Because Linux 0.12 is a monolithic kernel, every device driver (keyboard, TTY console, floppy, hard disk) and the entire VFS/MINIX filesystem are compiled in supervisor mode and statically linked into `tools/system`. By contrast, **ABOS Lesson 20** is a capability-based AArch64 microkernel modeled after Google Fuchsia's **Zircon**. Its build system enforces strict privilege and address-space separation at compile, link, and image-packaging time:

1. **Minimal `EL1` Microkernel + Embedded `userboot` (`image/kernel.img`)**: Only the `EL1` microkernel subsystems (`kernel/core/`, `kernel/mm/`, `kernel/dev/`), the root bootstrap process (`user/userboot/userboot.o`), and the thin syscall wrapper object (`user/ulibc/ulibc.o`) are linked via `linker.ld` into `image/kernel.elf` and stripped into `image/kernel.img`. Neither the console driver, nor the VirtIO-Blk filesystem driver, nor the interactive shell is linked into the kernel binary.
2. **Zircon-Style BOOTFS Initial RAM Disk (`image/bootfs.tar`)**: All core user-space system servers (`console-srv.bin`, `fs-srv.bin`, and `shell.bin`) are compiled as independent `EL0` programs, linked at virtual base address `0x20000000` via the shared user-space linker script `user/link.ld` against `crt0.o` and `libulibc.a`, and packed into an uncompressed POSIX USTAR archive (`bootfs.tar`) passed to QEMU via `-initrd`.
3. **VirtIO-Blk Persistent TAR Disk (`image/disk.img`)**: Standalone user-space applications in `user/apps/` (such as `calc.bin`) and text assets (`motd.txt`, `zircon.txt`) are packed into a 1 MB sector-aligned USTAR disk image (`disk.img`) attached to QEMU's VirtIO-MMIO block device and served dynamically over IPC by `fs-srv`.

This chapter presents a complete, 100% line-by-line commentary on all **16 build, configuration, and image-packaging files** (548 source lines in total) that turn raw C and AArch64 assembly sources into a bootable 4-core SMP microkernel system.

---

## 16.1 Main Functions and Build DAG

### 16.1.1 Architectural Separation of Compilation and Linkage Domains

Although every C and assembly translation unit in ABOS Lesson 20 is compiled with the same AArch64 freestanding cross-compiler flags defined in `config.mk`, the build pipeline partitions object files into **three distinct linkage domains**:

| Linkage Domain | Linker Script | Entry Symbol & Load/Link VA | Constituent Object Files | Final Output Artifact |
| :--- | :--- | :--- | :--- | :--- |
| **1. `EL1` Kernel + `userboot`** | `linker.ld` | `_start` at PA `0x40080000`; `.text` at VA `0xFFFFFFFF80084000` | `kernel/core/*.o`, `kernel/mm/*.o`, `kernel/dev/*.o`, `user/userboot/userboot.o`, `user/ulibc/ulibc.o` | `image/kernel.elf` -> `image/kernel.img` (`-kernel`) |
| **2. BOOTFS `EL0` Servers** | `user/link.ld` | `_start` (`crt0.o`) at VA `0x20000000` | `crt0.o` + `{console_srv,fs_srv,shell}.o` + `libulibc.a` | `image/{console-srv,fs-srv,shell}.bin` -> `image/bootfs.tar` (`-initrd`) |
| **3. Disk `EL0` Applications** | `user/link.ld` | `_start` (`crt0.o`) at VA `0x20000000` | `crt0.o` + `user/apps/<app>.o` + `libulibc.a` | `image/<app>.bin` -> `image/disk.img` (`-drive` VirtIO-Blk) |

Notice two subtle architectural details in this split:

* **Why `userboot` is linked into `kernel.elf` without `crt0.o`**: In Zircon (and ABOS), the kernel itself deliberately contains no filesystem driver and no general-purpose ELF/program loader. To break the chicken-and-egg bootstrap cycle, `userboot.o` and `ulibc.o` are linked into the kernel's read-only image pages and exposed to `EL0` via `USER_KVA_TO_UVA(userboot_entry)`. Because `userboot` executes out of read-only kernel image pages rather than a writable `.bss`-backed standalone VMO at `0x20000000`, it cannot run `crt0.S`'s `.bss`-zeroing loop or write to global variables in `crt_env.c`. Instead, `lk_main()` starts `userboot` directly at `userboot_entry(boot_ch)`, where all state lives on `userboot`'s private `EL0` user stack.
* **Why every standalone `EL0` server and app shares `0x20000000`**: Unlike early single-address-space RTOS designs that must assign non-overlapping link addresses to every task, ABOS isolates each `EL0` process inside its own `VmAspace` (`TTBR0_EL1` + hardware 16-bit ASID). Consequently, `console-srv.bin`, `fs-srv.bin`, `shell.bin` (instantiated four times as `abos-shell-0..3`), and `calc.bin` all link with the exact same `user/link.ld` script at `0x20000000`!

### 16.1.2 Complete Build Dependency Directed Acyclic Graph (DAG)

When a developer runs `make` (or `make run`) at the root of `lesson_20/`, GNU `make` evaluates the two-level recursive hierarchy shown below:

```text
                        +------------------------------+
                        |   ./install_tools.sh         |  (One-time host setup)
                        +--------------+---------------+
                                       | installs GCC, Binutils, QEMU, Python3
                                       v
                        +------------------------------+
                        |   config.mk (Shared Config)  |  Included by ALL 13 Makefiles
                        +--------------+---------------+
                                       |
                        +--------------v---------------+
                        |   Makefile (Top-Level Root)  |
                        |  all: check-tools            |
                        |       kernel user image      |
                        +---+----------+-----------+---+
                            |          |           |
        +-------------------+          |           +--------------------+
        |                              |                                |
        v                              v                                v
+---------------+              +---------------+                +---------------+
|kernel/Makefile|              | user/Makefile |                |image/Makefile |
+---+---+---+---+              +---+-------+---+                | (waits for    |
    |   |   |                      |       |                    | kernel & user)|
    |   |   |                      v       +--------+           +-------+-------+
    |   |   |             +----------------+        |                   |
    |   |   |             |ulibc/Makefile  |        | depends on ulibc  |
    |   |   |             | crt0.o         |        v                   |
    |   |   |             | crt_env.o      |  +-----+------------+      |
    |   |   |             | ulibc.o        |  | drivers/Makefile |      |
    |   |   |             | -> libulibc.a  |  | shell/Makefile   |      |
    |   |   |             +--------+-------+  | apps/Makefile    |      |
    v   v   v                      |          | (userboot/       |      |
+-------+-------+                  |          |  Makefile -> .o) |      |
|core/*.o       |                  |          +--------+---------+      |
|mm/*.o         |                  |                   |                |
|dev/*.o        |                  |     [LD user/link.ld @ 0x20000000] |
+-------+-------+                  |     [OBJCOPY -O binary]            |
        |                          |                   |                |
        |                          |                   v                |
        |                          |     image/console-srv.{elf,bin}    |
        |                          |     image/fs-srv.{elf,bin}         |
        |                          |     image/shell.{elf,bin}          |
        |                          |     image/calc.{elf,bin}           |
        |                          |                   |                |
        +--------------------------+-------------------+----------------+
                                                       |
                +--------------------------------------+------------------------+
                |                                      |                        |
                v                                      v                        v
  [LD linker.ld + OBJCOPY]                 [python3 make_bootfs.py] [python3 make_tar_disk.py]
  kernel/*.o + userboot.o + ulibc.o        console-srv + fs-srv     motd.txt + zircon.txt
                |                          + shell.bin              + calc.bin (padded 1 MB)
                v                                      |                        |
       image/kernel.elf                                v                        v
       image/kernel.img (-kernel)             image/bootfs.tar         image/disk.img
                                                 (-initrd)             (-drive VirtIO-Blk)
```

### 16.1.3 Inventory of All 16 Build, Setup, and Packaging Files

Table 16-1 summarizes every build-related script and `Makefile` in `lesson_20/` and its corresponding program listing in this chapter.

| Listing | File Path (`lesson_20/...`) | Lines | Subsystem Role |
| :--- | :--- | :---: | :--- |
| **Program 16-1** | `install_tools.sh` | 69 | Cross-platform installer for AArch64 GCC, Binutils, QEMU, and Python 3 |
| **Program 16-2** | `config.mk` | 64 | Shared toolchain auto-discovery, directory paths, `CFLAGS`, `LDFLAGS`, and `.c`/`.S` pattern rules |
| **Program 16-3** | `Makefile` | 67 | Root orchestrator (`all`, `check-tools`, `kernel`, `user`, `image`, `run`, `clean`) |
| **Program 16-4** | `kernel/Makefile` | 19 | Dispatches recursive builds across `EL1` kernel subdirectories (`core`, `mm`, `dev`) |
| **Program 16-5** | `kernel/core/Makefile` | 17 | Compiles `EL1` boot assembly, exception vectors, SMP scheduler, objects, and syscalls |
| **Program 16-6** | `kernel/mm/Makefile` | 16 | Compiles `EL1` Physical Memory Manager (`pmm.o`), kernel heap (`heap.o`), and MMU (`mmu.o`) |
| **Program 16-7** | `kernel/dev/Makefile` | 16 | Compiles `EL1` GICv2 (`gic.o`), ARM Generic Timer (`timer.o`), and early UART debuglog (`debuglog.o`) |
| **Program 16-8** | `user/Makefile` | 22 | Dispatches `EL0` builds and enforces `drivers shell apps: ulibc` prerequisite ordering |
| **Program 16-9** | `user/ulibc/Makefile` | 20 | Compiles `crt0.o`, `crt_env.o`, `ulibc.o` and archives static library `libulibc.a` |
| **Program 16-10** | `user/userboot/Makefile` | 16 | Compiles `userboot.o` for direct embedding into the kernel image |
| **Program 16-11** | `user/drivers/Makefile` | 36 | Links and strips standalone `EL0` servers `console-srv.bin` and `fs-srv.bin` at `0x20000000` |
| **Program 16-12** | `user/shell/Makefile` | 32 | Links and strips standalone `EL0` capability shell `shell.bin` at `0x20000000` |
| **Program 16-13** | `user/apps/Makefile` | 37 | Auto-discovers `user/apps/*.c` and builds `image/<app>.elf` and `image/<app>.bin` at `0x20000000` |
| **Program 16-14** | `image/Makefile` | 59 | Links `kernel.elf`/`kernel.img` and invokes Python USTAR builders for `bootfs.tar` and `disk.img` |
| **Program 16-15** | `image/make_bootfs.py` | 19 | Packs `console-srv.bin`, `fs-srv.bin`, and `shell.bin` into POSIX USTAR `bootfs.tar` |
| **Program 16-16** | `image/make_tar_disk.py` | 39 | Packs `motd.txt`, `zircon.txt`, and `user/apps/*.bin` into 1 MB raw USTAR `disk.img` |

---

## 16.2 Toolchain Setup and Root Build Configuration

The root directory of `lesson_20/` contains three foundational files that prepare the host environment, centralize compiler/linker flags, and drive the top-level build and QEMU simulation lifecycle: `install_tools.sh`, `config.mk`, and `Makefile`.

### 16.2.1 Cross-Toolchain Installer: `install_tools.sh`

#### Functional Description
Because most developers edit and build operating systems on `x86_64` Linux workstations or Apple Silicon / Intel macOS laptops, building ABOS requires a cross-compilation toolchain targeting 64-bit ARM (`aarch64`), the `qemu-system-aarch64` full-system emulator, GNU `make`, and `python3` (used to construct deterministic POSIX USTAR archives without host `tar` uid/gid leakage). `install_tools.sh` detects the host operating system's package manager (`apt-get`, `dnf`, `pacman`, or Homebrew `brew`), installs the exact required packages, and prints the installed tool versions to confirm readiness.

#### Heavily Commented Source Code

#### Program 16-1: `lesson_20/install_tools.sh`

<!-- ECLK_CODE_LISTING file="lesson_20/install_tools.sh" start="1" end="69" total="69" -->
```bash
       ## Use /usr/bin/env to locate bash portably across Linux distributions and macOS
       ## (where a newer bash may reside under /opt/homebrew/bin rather than /bin/bash).
   1 | #!/usr/bin/env bash
   2 | # =========================================================================
   3 | # install_tools.sh - Install AArch64 Cross-Compiler, Binutils, QEMU & Python3
   4 | #
   5 | # Supports:
   6 | #   - Debian / Ubuntu / gLinux (apt-get)
   7 | #   - Fedora / RHEL / CentOS   (dnf / yum)
   8 | #   - Arch Linux               (pacman)
   9 | #   - macOS                    (Homebrew)
  10 | # =========================================================================
       ## Enable fail-fast execution ('set -e' / errexit): if any package manager
       ## update, installation command, or verification step returns a non-zero exit
       ## code, abort the script immediately instead of printing a misleading success
       ## banner at the end.
  11 | set -e
  12 | 
  13 | echo "=================================================================="
  14 | echo "  ABOS Toolchain & QEMU Installer"
  15 | echo "=================================================================="
  16 | 
       ## Probe the host PATH with the POSIX shell builtin 'command -v' (suppressing
       ## stdout and stderr) to identify the system package manager.
       ## Branch 1 — Debian, Ubuntu, and Google gLinux (apt-get): note the Debian
       ## packaging subtlety on line 24 where 'qemu-system-aarch64' is shipped inside
       ## the 'qemu-system-arm' binary package rather than a standalone package name.
  17 | if command -v apt-get >/dev/null 2>&1; then
  18 |     echo "[*] Detected Debian/Ubuntu/gLinux (apt-get). Installing packages..."
  19 |     sudo apt-get update
  20 |     sudo apt-get install -y \
  21 |         build-essential \
  22 |         gcc-aarch64-linux-gnu \
  23 |         binutils-aarch64-linux-gnu \
  24 |         qemu-system-arm \
  25 |         python3
       ## Branch 2 — Fedora, RHEL, CentOS, and Rocky Linux (dnf): Fedora packages the
       ## 64-bit ARM system emulator separately as 'qemu-system-aarch64' and provides a
       ## bare-metal-capable 'gcc-aarch64-linux-gnu' cross-compiler.
  26 | elif command -v dnf >/dev/null 2>&1; then
  27 |     echo "[*] Detected Fedora/RHEL (dnf). Installing packages..."
  28 |     sudo dnf install -y \
  29 |         make \
  30 |         gcc-aarch64-linux-gnu \
  31 |         binutils-aarch64-linux-gnu \
  32 |         qemu-system-aarch64 \
  33 |         python3
       ## Branch 3 — Arch Linux (pacman): Arch prefixes cross-toolchain package names
       ## with the target triplet ('aarch64-linux-gnu-gcc',
       ## 'aarch64-linux-gnu-binutils') and uses '--noconfirm' for unattended
       ## installation.
  34 | elif command -v pacman >/dev/null 2>&1; then
  35 |     echo "[*] Detected Arch Linux (pacman). Installing packages..."
  36 |     sudo pacman -Sy --noconfirm \
  37 |         base-devel \
  38 |         aarch64-linux-gnu-gcc \
  39 |         aarch64-linux-gnu-binutils \
  40 |         qemu-system-aarch64 \
  41 |         python
       ## Branch 4 — macOS with Homebrew (brew): macOS does not ship ELF binutils
       ## natively, so Homebrew's bare-metal 'aarch64-elf-gcc' formula (which pulls in
       ## 'aarch64-elf-binutils') is installed alongside 'qemu', 'python3', and GNU
       ## 'make'.
  42 | elif command -v brew >/dev/null 2>&1; then
  43 |     echo "[*] Detected macOS (Homebrew). Installing packages..."
  44 |     brew install aarch64-elf-gcc qemu python3 make
       ## Fallback error handler for unrecognized hosts (e.g., NixOS, Gentoo, Alpine, or
       ## Windows WSL without standard managers): print exact manual requirements and
       ## return non-zero status 1.
  45 | else
  46 |     echo "[!] Unsupported package manager. Please manually install:"
  47 |     echo "    - aarch64-linux-gnu-gcc (or aarch64-elf-gcc)"
  48 |     echo "    - aarch64-linux-gnu-binutils"
  49 |     echo "    - qemu-system-aarch64"
  50 |     echo "    - python3 & make"
  51 |     exit 1
  52 | fi
  53 | 
  54 | echo ""
  55 | echo "=================================================================="
  56 | echo "  Verifying Installed Tools:"
  57 | echo "=================================================================="
       ## Post-install verification block: print the first line of '--version' output
       ## for whichever AArch64 GCC prefix was installed ('aarch64-linux-gnu-gcc' on
       ## Linux or 'aarch64-elf-gcc' on macOS), followed by QEMU and Python 3.
  58 | if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  59 |     aarch64-linux-gnu-gcc --version | head -n 1
  60 | elif command -v aarch64-elf-gcc >/dev/null 2>&1; then
  61 |     aarch64-elf-gcc --version | head -n 1
  62 | fi
  63 | 
  64 | if command -v qemu-system-aarch64 >/dev/null 2>&1; then
  65 |     qemu-system-aarch64 --version | head -n 1
  66 | fi
  67 | 
  68 | python3 --version
  69 | echo "[+] All ABOS build & simulation tools are ready! Run 'make run' now."
       ## Once install_tools.sh finishes cleanly, config.mk will automatically discover
       ## the installed cross-compiler and QEMU binary on every subsequent 'make'
       ## invocation.
```

#### Technical Notes
* **Why `aarch64-linux-gnu-gcc` and `aarch64-elf-gcc` produce identical ABOS binaries**: On Linux hosts, distributions universally package `aarch64-linux-gnu-gcc` (whose default user-space target assumes glibc), whereas macOS Homebrew provides bare-metal `aarch64-elf-gcc`. When invoked with `-ffreestanding -nostdlib -nostartfiles -fno-pie -fno-pic` (see `config.mk` lines 50–52) and linked directly via `ld -n -T <script.ld>`, all OS-specific C runtime startups (`crt1.o`), dynamic linker sections (`.interp`, `.got`, `.plt`), and standard libraries (`libc`, `libgcc`) are completely bypassed. Both compilers emit pure AAPCS64 machine code against the exact same ELF64 little-endian AArch64 object format (`elf64-littleaarch64`).

---

### 16.2.2 Shared Build Configuration: `config.mk`

#### Functional Description
`config.mk` is the single source of truth for the entire ABOS build system. Included at the top of the root `Makefile` and all 12 subdirectory `Makefile`s, it dynamically resolves `ROOT_DIR`, auto-detects the cross-compiler prefix (`CROSS_COMPILE`) and `QEMU` executable across system paths and Zircon's prebuilt tree, defines canonical paths for headers, linker scripts, and `ulibc` artifacts, specifies bare-metal AArch64 `CFLAGS` and `LDFLAGS`, and provides quiet, formatted pattern rules (`[CC]` and `[AS]`) for compiling `.c` and `.S` sources.

#### Heavily Commented Source Code

#### Program 16-2: `lesson_20/config.mk`

<!-- ECLK_CODE_LISTING file="lesson_20/config.mk" start="1" end="64" total="64" -->
```makefile
   1 | # =========================================================================
   2 | # config.mk - Shared Toolchain & Build Configuration for ABOS Lesson 20
   3 | #
   4 | # Included by the top-level Makefile and every subdirectory Makefile.
   5 | # =========================================================================
   6 | 
       ## Self-locating root directory idiom: whenever GNU make parses an 'include
       ## .../config.mk' directive, the path of config.mk itself is appended as the last
       ## element of $(MAKEFILE_LIST). Taking $(abspath $(dir $(lastword
       ## $(MAKEFILE_LIST)))) resolves the absolute path to lesson_20/ regardless of
       ## which subdirectory Makefile invoked 'make'!
       ## Lines 8-10 compute fallback paths into a sibling Zircon checkout
       ## (zircon_org/prebuilt/downloads) so developers working inside a full
       ## Zircon/ABOS workspace can build and run out-of-the-box even without root
       ## package installation.
   7 | ROOT_DIR      ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
   8 | ZIRCON_PRE    := $(abspath $(ROOT_DIR)/../../../zircon_org/prebuilt/downloads)
   9 | PREBUILT_GCC  := $(ZIRCON_PRE)/gcc/bin/aarch64-elf-gcc
  10 | PREBUILT_QEMU := $(ZIRCON_PRE)/qemu/bin/qemu-system-aarch64
  11 | 
       ## Four-tier cross-compiler prefix auto-detection (using '?=' so a user can
       ## always override via 'make CROSS_COMPILE=clang-...' on the command line):
       ##   1. System 'aarch64-linux-gnu-' (Debian/Ubuntu/Fedora/Arch)
       ##   2. System 'aarch64-elf-' (macOS Homebrew / custom bare-metal toolchains)
       ##   3. Sibling Zircon prebuilt 'aarch64-elf-' toolchain
       ##   4. Default 'aarch64-linux-gnu-' (caught cleanly by 'make check-tools' if missing)
  12 | ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
  13 |     CROSS_COMPILE ?= aarch64-linux-gnu-
  14 | else ifneq ($(shell which aarch64-elf-gcc 2>/dev/null),)
  15 |     CROSS_COMPILE ?= aarch64-elf-
  16 | else ifneq ($(wildcard $(PREBUILT_GCC)),)
  17 |     CROSS_COMPILE ?= $(ZIRCON_PRE)/gcc/bin/aarch64-elf-
  18 | else
  19 |     CROSS_COMPILE ?= aarch64-linux-gnu-
  20 | endif
  21 | 
       ## Two-tier QEMU emulator auto-detection: prefer 'qemu-system-aarch64' on the
       ## host PATH, falling back to Zircon's prebuilt QEMU binary if present.
  22 | ifneq ($(shell which qemu-system-aarch64 2>/dev/null),)
  23 |     QEMU ?= qemu-system-aarch64
  24 | else ifneq ($(wildcard $(PREBUILT_QEMU)),)
  25 |     QEMU ?= $(PREBUILT_QEMU)
  26 | else
  27 |     QEMU ?= qemu-system-aarch64
  28 | endif
  29 | 
       ## Bind the four GNU binutils/compiler drivers using immediate ':=' expansion:
       ##   - CC      : C compiler and assembly preprocessor driver (gcc)
       ##   - LD      : Standalone ELF linker (ld, invoked directly rather than via gcc)
       ##   - AR      : Static library archiver (ar, builds libulibc.a)
       ##   - OBJCOPY : Binary translator (objcopy -O binary, strips ELF headers to raw flat images)
  30 | CC            := $(CROSS_COMPILE)gcc
  31 | LD            := $(CROSS_COMPILE)ld
  32 | AR            := $(CROSS_COMPILE)ar
  33 | OBJCOPY       := $(CROSS_COMPILE)objcopy
  34 | 
       ## Canonical project-wide directory and file variables:
       ##   - INC_DIR   : Shared kernel/user ABI headers (syscalls.h, object.h, mmu.h, ipc_proto.h)
       ##   - ULIBC_DIR : EL0 C runtime and static syscall library directory
       ##   - IMAGE_DIR : Staging & output directory for all .elf, .bin, .tar, and .img artifacts
       ##   - KERNEL_LD : Higher-half EL1 linker script (lesson_20/linker.ld)
       ##   - USER_LD   : Common VA 0x20000000 EL0 linker script (lesson_20/user/link.ld)
  35 | # Shared directories & artifacts
  36 | INC_DIR       := $(ROOT_DIR)/include
  37 | ULIBC_DIR     := $(ROOT_DIR)/user/ulibc
  38 | IMAGE_DIR     := $(ROOT_DIR)/image
  39 | 
  40 | KERNEL_LD     := $(ROOT_DIR)/linker.ld
  41 | USER_LD       := $(ROOT_DIR)/user/link.ld
  42 | 
       ## Explicit paths to user/ulibc build products: CRT0_OBJ (crt0.o) provides _start
       ## for standalone EL0 executables; ULIBC_OBJ (ulibc.o) is linked directly into
       ## kernel.elf for userboot; ULIBC_A (libulibc.a) bundles crt0.o + crt_env.o +
       ## ulibc.o for standalone EL0 servers, shells, and apps.
  43 | CRT0_OBJ      := $(ULIBC_DIR)/crt0.o
  44 | CRT_ENV_OBJ   := $(ULIBC_DIR)/crt_env.o
  45 | ULIBC_OBJ     := $(ULIBC_DIR)/ulibc.o
  46 | ULIBC_A       := $(ULIBC_DIR)/libulibc.a
  47 | 
       ## Conservative header dependency tracking: collecting every *.h file in include/
       ## and user/ulibc/ into $(HEADERS) ensures that editing any struct layout,
       ## syscall number, or inline helper automatically triggers recompilation of every
       ## .o file.
  48 | HEADERS       := $(wildcard $(INC_DIR)/*.h) $(wildcard $(ULIBC_DIR)/*.h)
  49 | 
       ## Core AArch64 compilation flags (CFLAGS):
       ##   -O0                  : Disable optimization so source lines map 1-to-1 to assembly in GDB
       ##   -Wall -Wextra        : Strict compile-time diagnostics
       ##   -nostdlib -nostartfiles -ffreestanding : Freestanding environment; prohibit host libc
       ##   -mcpu=cortex-a53     : Target in-order ARMv8-A Cortex-A53 pipeline
       ##   -mgeneral-regs-only  : CRITICAL for OS kernels! Forbids compiler-generated NEON/FP registers (q0..q31/v0..v31) for memcpy/struct assignments, avoiding lazy FP context-switch corruption in EL1
       ##   -fno-pie -fno-pic    : Emit absolute/PC-relative addressing without a Global Offset Table (.got), essential because both kernel.img and user .bin files are loaded as raw binary images without dynamic relocation
       ##   -g                   : Emit full DWARF debugging symbols into .o and .elf files
       ##   -I$(INC_DIR) -I$(ULIBC_DIR) : Unified header search paths
  50 | CFLAGS        := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
  51 |                  -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g \
  52 |                  -I$(INC_DIR) -I$(ULIBC_DIR)
  53 | 
       ## Linker flags: '-n' (--nmagic) disables automatic 64 KB ELF segment
       ## page-padding inside GNU ld for AArch64, keeping standalone EL0 .elf files
       ## compact while strictly honoring section alignments in $(KERNEL_LD) and
       ## $(USER_LD).
  54 | KERNEL_LDFLAGS := -n -T $(KERNEL_LD)
  55 | USER_LDFLAGS   := -n -T $(USER_LD)
  56 | 
       ## Shared implicit pattern rules for C (%.c -> %.o) and preprocessed AArch64
       ## assembly (%.S -> %.o). Passing %.S through $(CC) rather than raw 'as' runs the
       ## C preprocessor first and preserves DWARF line debug info (-g) for assembly
       ## instructions!
  57 | # Standard compilation rules for C and Assembly sources
  58 | %.o: %.c $(HEADERS)
  59 | 	@echo "  [CC]      $(patsubst $(ROOT_DIR)/%,%,$(abspath $<))"
  60 | 	@$(CC) $(CFLAGS) -c $< -o $@
  61 | 
  62 | %.o: %.S $(HEADERS)
  63 | 	@echo "  [AS]      $(patsubst $(ROOT_DIR)/%,%,$(abspath $<))"
  64 | 	@$(CC) $(CFLAGS) -c $< -o $@
```

#### Technical Notes
* **Why `-mgeneral-regs-only` is mandatory in AArch64 microkernels**: On AArch64, GCC at any optimization level (and even `-O0` for variadic functions like `dprintf`, `u_vsnprintf`, or large struct zeroing) will emit 128-bit SIMD/FP `stp q0, q1, [sp, ...]` instructions unless `-mgeneral-regs-only` is specified. Notice `exceptions.h:iframe_t` saves general-purpose registers `x0..x30`, `sp_el0`, `elr_el1`, `spsr_el1`, and `mdscr_el1` (288 bytes), not the 512-byte NEON `v0..v31` register bank. Compiling with `-mgeneral-regs-only` guarantees neither the `EL1` kernel nor `EL0` servers clobber SIMD registers across syscalls or timer preemptions.
* **Why `-n` (`--nmagic`) matters on AArch64 `ld`**: By default, `aarch64-linux-gnu-ld` sets `COMMONPAGESIZE` / `MAXPAGESIZE` to `0x10000` (64 KB) so Linux ELF binaries can run on both 4 KB and 64 KB granule kernels. Without `-n`, `ld` pads ELF segments to 64 KB boundaries. Passing `-n` tells `ld` to lay out sections strictly according to `linker.ld` (4 KB aligned) and `user/link.ld` (8-byte aligned), keeping `calc.bin` down to 11 KB instead of 68+ KB.

---

### 16.2.3 Top-Level Orchestrator: `Makefile`

#### Functional Description
The root `Makefile` provides the developer-facing entry points for building, verifying tools, running in QEMU, and cleaning the workspace. It enforces a strict three-stage top-level build order—`kernel` and `user` compile all `EL1` and `EL0` components first, and `image` runs only after both complete—before `make run` boots the resulting three-artifact system on a 4-core QEMU `virt` machine.

#### Heavily Commented Source Code

#### Program 16-3: `lesson_20/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/Makefile" start="1" end="67" total="67" -->
```makefile
       ## Top-level architecture banner documenting the entire Lesson 20 directory
       ## layout so a new reader opening lesson_20/Makefile immediately sees how EL1
       ## kernel code, EL0 user-space servers, and final image builders fit together.
   1 | # =========================================================================
   2 | # ABOS Lesson 20: Standalone EL0 System Servers via Zircon-Style BOOTFS
   3 | #
   4 | # Directory & Build Hierarchy:
   5 | #   config.mk                - Shared toolchain, flags, paths & compilation rules
   6 | #   linker.ld                - EL1 higher-half microkernel linker script
   7 | #   include/                 - Shared kernel, MM, object, syscall, and IPC headers
   8 | #   kernel/Makefile          - Orchestrates EL1 kernel subdirectories:
   9 | #     kernel/core/Makefile   - Boot ASM, exceptions, SMP scheduler, objects, syscalls
  10 | #     kernel/mm/Makefile     - Physical Memory Manager (pmm), heap, 4-level MMU
  11 | #     kernel/dev/Makefile    - GICv2 interrupt controller, timer, early debuglog
  12 | #   user/Makefile            - Orchestrates EL0 user-space subdirectories:
  13 | #     user/link.ld           - Common linker script shared by ALL EL0 executables
  14 | #     user/ulibc/Makefile    - C Runtime (crt0.S, crt_env.c) & libulibc.a
  15 | #     user/drivers/Makefile  - Standalone EL0 Drivers (console-srv.bin, fs-srv.bin)
  16 | #     user/userboot/Makefile - EL0 root bootstrap process (embedded in kernel)
  17 | #     user/shell/Makefile    - Standalone EL0 Shell (shell.bin)
  18 | #     user/apps/Makefile     - Standalone EL0 Apps (calc.bin)
  19 | #   image/Makefile           - Packs bootfs.tar, links kernel.img, and packs disk.img
  20 | # =========================================================================
  21 | 
       ## Compute ROOT_DIR explicitly before including config.mk, then define the three
       ## top-level build subdirectories (kernel, user, image) and the three bootable
       ## output paths in image/.
  22 | ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
  23 | include $(ROOT_DIR)/config.mk
  24 | 
  25 | SUBDIRS    := kernel user image
  26 | KERNEL_IMG := $(IMAGE_DIR)/kernel.img
  27 | BOOTFS_TAR := $(IMAGE_DIR)/bootfs.tar
  28 | DISK_IMG   := $(IMAGE_DIR)/disk.img
  29 | 
       ## Declare all lifecycle targets .PHONY so GNU make always executes their recipes
       ## even though directories named 'kernel', 'user', and 'image' physically exist
       ## on disk! Without '.PHONY: kernel user image', make would see that those
       ## directories already exist and have no newer prerequisites, and silently skip
       ## building them.
  30 | .PHONY: all kernel user image run clean check-tools install-tools
  31 | 
       ## Default goal 'all': validates the cross-compiler first, builds kernel and user
       ## subtrees, and packages final images.
  32 | all: check-tools kernel user image
  33 | 
       ## Friendly pre-flight guard ('check-tools'): verifies that $(CC) is either on
       ## PATH ('command -v') or an executable absolute path ('[ -x ... ]', e.g.,
       ## Zircon's prebuilt compiler) and directs the user to './install_tools.sh' if
       ## missing.
  34 | check-tools:
  35 | 	@if ! command -v $(CC) >/dev/null 2>&1 && [ ! -x "$(CC)" ]; then \
  36 | 		echo "Error: Cross-compiler '$(CC)' not found!"; \
  37 | 		echo "Run './install_tools.sh' (or 'make install-tools') to install AArch64 GCC & QEMU."; \
  38 | 		exit 1; \
  39 | 	fi
  40 | 
  41 | install-tools:
  42 | 	./install_tools.sh
  43 | 
       ## Stage 1a & 1b ('kernel' and 'user'): invoke recursive make in kernel/ and
       ## user/ with '--no-print-directory' to keep terminal build logs clean and
       ## readable. Note that under parallel 'make -j', 'kernel' and 'user' can safely
       ## run concurrently because their object outputs are completely disjoint.
  44 | kernel: check-tools
  45 | 	@$(MAKE) --no-print-directory -C kernel all
  46 | 
  47 | user: check-tools
  48 | 	@$(MAKE) --no-print-directory -C user all
  49 | 
       ## Stage 2 ('image: kernel user'): crucial synchronization barrier!
       ## image/Makefile links kernel/*.o + userboot.o + ulibc.o into kernel.elf and
       ## packs console-srv.bin, fs-srv.bin, shell.bin, and calc.bin into TAR archives,
       ## so it MUST wait until both 'kernel' and 'user' have finished.
  50 | image: kernel user
  51 | 	@$(MAKE) --no-print-directory -C image all
  52 | 
       ## Simulation target ('run: all'): ensures all binaries and disk archives are
       ## up-to-date, checks that $(QEMU) is installed, and launches a 4-core Cortex-A53
       ## QEMU 'virt' machine with -kernel kernel.img, -initrd bootfs.tar, and a raw
       ## VirtIO-Blk drive backed by disk.img (examined in detail in Section 17.1).
  53 | run: all
  54 | 	@if ! command -v $(QEMU) >/dev/null 2>&1 && [ ! -x "$(QEMU)" ]; then \
  55 | 		echo "Error: '$(QEMU)' not found! Run './install_tools.sh' first."; \
  56 | 		exit 1; \
  57 | 	fi
  58 | 	@echo "Running ABOS Lesson 20 in QEMU..."
  59 | 	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel $(KERNEL_IMG) -initrd $(BOOTFS_TAR) \
  60 | 		-nographic -serial mon:stdio \
  61 | 		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0
  62 | 
       ## Workspace cleanup ('clean'): iterates over kernel, user, and image to remove
       ## all compiled objects, archives, ELF binaries, flat binaries, and disk images.
  63 | clean:
  64 | 	@echo "Cleaning up all subdirectory build artifacts..."
  65 | 	@for dir in $(SUBDIRS); do \
  66 | 		$(MAKE) --no-print-directory -C $$dir clean; \
  67 | 	done
```

#### Technical Notes
* **Why `.PHONY` is essential when target names match subdirectory names**: In POSIX filesystems, `lesson_20/kernel`, `lesson_20/user`, and `lesson_20/image` exist as directories. In GNU `make`, a non-phony rule `kernel: check-tools` would check whether a file or directory named `kernel` exists; finding the directory `lesson_20/kernel/`, `make` would consider the target already satisfied unless `check-tools` forced an update. Declaring `.PHONY: all kernel user image ...` instructs `make` to treat them purely as action names, guaranteeing recursive descent into each subdirectory on every invocation.

---

## 16.3 Kernel Subdirectory Makefiles

In ABOS Lesson 20, the `EL1` microkernel source tree under `kernel/` is factored into three clean subsystems mirroring Zircon's `kernel/` layout:
* `kernel/core/`: Architecture boot (`start.S`), exception vectors & syndrome dispatch (`exceptions.S`, `exceptions_c.c`), thread context switching & EL0 transition (`switch.S`, `uspace_entry.S`, `thread.c`, `wait.c`), FDT parser (`dtb.c`), capability dispatchers (`object.c`), syscall table (`syscalls.c`), and `lk_main` (`main.c`).
* `kernel/mm/`: Physical Memory Manager (`pmm.c`), freelist kernel heap (`heap.c`), and 3-level ASID-tagged AArch64 MMU (`mmu.c`).
* `kernel/dev/`: GICv2 interrupt controller (`gic.c`), ARMv8 Generic Timer (`timer.c`), and spinlock-serialized early PL011 debuglog (`debuglog.c`).

Each leaf kernel `Makefile` compiles its `.c` and `.S` files into `.o` object files in place; final linkage is deferred to `image/Makefile`.

### 16.3.1 Kernel Subsystem Aggregator: `kernel/Makefile`

#### Functional Description
`kernel/Makefile` fans out `all` and `clean` operations across the three `EL1` kernel subdirectories (`core`, `mm`, and `dev`).

#### Heavily Commented Source Code

#### Program 16-4: `lesson_20/kernel/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/Makefile" start="1" end="19" total="19" -->
```makefile
   1 | # =========================================================================
   2 | # kernel/Makefile - EL1 Microkernel Subsystems (core, mm, dev)
   3 | # =========================================================================
       ## Allow standalone invocation ('cd kernel && make') by defaulting ROOT_DIR to
       ## '$(abspath ..)' when not inherited from the parent make, then pull in shared
       ## compiler flags and rules from config.mk.
   4 | ROOT_DIR ?= $(abspath ..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## List the three orthogonal EL1 kernel subsystems: core, mm, and dev. Because
       ## none of these subdirectories generates headers or archives consumed by the
       ## others at compile time, they have zero inter-directory build order
       ## dependencies.
   7 | SUBDIRS := core mm dev
   8 | 
       ## Mark all, clean, and every element of $(SUBDIRS) as .PHONY so '$(SUBDIRS):' on
       ## line 13 acts as a multi-target pattern that spawns a sub-make ('-C $@ all')
       ## for each directory—potentially in parallel under 'make -j3'.
   9 | .PHONY: all clean $(SUBDIRS)
  10 | 
  11 | all: $(SUBDIRS)
  12 | 
  13 | $(SUBDIRS):
  14 | 	@$(MAKE) --no-print-directory -C $@ all
  15 | 
       ## Clean rule: sequentially invoke 'clean' in core, mm, and dev via a POSIX shell
       ## loop (note '$$dir' escapes '$' past make to the shell).
  16 | clean:
  17 | 	@for dir in $(SUBDIRS); do \
  18 | 		$(MAKE) --no-print-directory -C $$dir clean; \
  19 | 	done
```

#### Technical Notes
* **Target-based fan-out (`$(SUBDIRS):`) vs shell `for`-loop**: Notice how `all` uses make-level prerequisites (`all: $(SUBDIRS)` with `$(SUBDIRS): @$(MAKE) -C $@ all`) rather than a shell `for` loop. Using separate make targets allows GNU `make`'s jobserver (`make -j`) to build `core`, `mm`, and `dev` simultaneously across multiple host CPU cores, whereas a shell `for` loop (used only for the instantaneous `rm -f` in `clean`) runs strictly serially.

---

### 16.3.2 Core Kernel Rules: `kernel/core/Makefile`

#### Functional Description
`kernel/core/Makefile` discovers and compiles all C and AArch64 assembly translation units in `kernel/core/` (7 `.c` files and 4 `.S` files, yielding 11 `.o` object files) using the shared pattern rules inherited from `config.mk`.

#### Heavily Commented Source Code

#### Program 16-5: `lesson_20/kernel/core/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/core/Makefile" start="1" end="17" total="17" -->
```makefile
   1 | # =========================================================================
   2 | # kernel/core/Makefile - EL1 Boot, Exceptions, SMP Scheduler, Objects, Syscalls
   3 | # =========================================================================
       ## Resolve ROOT_DIR two levels up ('../..') when invoked directly inside
       ## kernel/core/, and include config.mk to inherit $(CC), $(CFLAGS), $(HEADERS),
       ## and the %.c/%.S -> %.o compilation rules.
   4 | ROOT_DIR ?= $(abspath ../..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## Discover all C sources (dtb.c, exceptions_c.c, main.c, object.c, syscalls.c,
       ## thread.c, wait.c) and assembly sources (exceptions.S, start.S, switch.S,
       ## uspace_entry.S), substituting suffixes to construct the 11-object $(OBJS)
       ## list.
   7 | C_SRCS := $(wildcard *.c)
   8 | S_SRCS := $(wildcard *.S)
   9 | OBJS   := $(C_SRCS:.c=.o) $(S_SRCS:.S=.o)
  10 | 
  11 | .PHONY: all clean
  12 | 
       ## Require all $(OBJS). The '@:' recipe invokes the POSIX shell no-op ':'
       ## silently; this suppresses GNU make's noisy 'make[2]: Nothing to be done for
       ## all.' message on incremental rebuilds when all .o files are already
       ## up-to-date!
  13 | all: $(OBJS)
  14 | 	@:
  15 | 
       ## Remove all compiled .o files in kernel/core/.
  16 | clean:
  17 | 	@rm -f $(OBJS)
```

#### Technical Notes
* **Why `@:` is used in leaf `all` rules**: In GNU `make`, if a phony target like `all: $(OBJS)` has *no* recipe commands at all and every prerequisite `.o` is already newer than its `.c`/`.S` and `$(HEADERS)`, `make` prints `Nothing to be done for 'all'` for every single leaf directory. Adding the single-character shell null utility `@:` gives `all` a zero-cost silent recipe so incremental `make` runs stay completely clean.

---

### 16.3.3 Memory Management Rules: `kernel/mm/Makefile`

#### Functional Description
`kernel/mm/Makefile` compiles the three `EL1` memory management modules: `pmm.c` (Zircon-style `vm_page_t` arena allocator), `heap.c` (512 KB coalescing freelist `kmalloc`/`kzalloc`/`kfree`), and `mmu.c` (3-level `TTBR0_EL1` user `VmAspace` manager and 16-bit ASID allocator).

#### Heavily Commented Source Code

#### Program 16-6: `lesson_20/kernel/mm/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/mm/Makefile" start="1" end="16" total="16" -->
```makefile
   1 | # =========================================================================
   2 | # kernel/mm/Makefile - EL1 Physical Memory Manager, Heap & 4-Level MMU
   3 | # =========================================================================
       ## Include shared build settings from lesson_20/config.mk.
   4 | ROOT_DIR ?= $(abspath ../..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## Collect pmm.c, heap.c, and mmu.c into C_SRCS and derive OBJS := pmm.o heap.o
       ## mmu.o. Unlike kernel/core/, kernel/mm/ is 100% C (inline assembly is used in
       ## mmu.c for 'tlbi' and 'dc cvau' / 'ic ialluis' cache maintenance), so no S_SRCS
       ## variable is needed.
   7 | C_SRCS := $(wildcard *.c)
   8 | OBJS   := $(C_SRCS:.c=.o)
   9 | 
  10 | .PHONY: all clean
  11 | 
       ## Build pmm.o, heap.o, and mmu.o via config.mk's '%.o: %.c $(HEADERS)' rule, and
       ## clean them on 'make clean'.
  12 | all: $(OBJS)
  13 | 	@:
  14 | 
  15 | clean:
  16 | 	@rm -f $(OBJS)
```

#### Technical Notes
* **Header-driven rebuild safety**: Because `config.mk` attaches `$(HEADERS)` (which includes `include/pmm.h`, `include/heap.h`, and `include/mmu.h`) to `%.o: %.c`, changing `KERNEL_PHYSMAP_OFFSET`, `PTE_*` descriptor flags, or `PMM_MAX_PAGES` automatically recompiles `pmm.o`, `heap.o`, and `mmu.o` alongside `kernel/core/*.o`.

---

### 16.3.4 Device Subsystem Rules: `kernel/dev/Makefile`

#### Functional Description
`kernel/dev/Makefile` compiles the minimal `EL1` platform hardware support required by a microkernel: `gic.c` (ARM GICv2 distributor and CPU interface), `timer.c` (100 Hz per-CPU ARM Generic Timer tick driving preemptive scheduling), and `debuglog.c` (interrupt-safe `dprintf` over the higher-half periphmap PL011 UART).

#### Heavily Commented Source Code

#### Program 16-7: `lesson_20/kernel/dev/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/kernel/dev/Makefile" start="1" end="16" total="16" -->
```makefile
       ## Notice what is intentionally ABSENT from kernel/dev/: there is no console line
       ## discipline, no virtual terminal multiplexer, no VirtIO block driver, and no
       ## TAR filesystem! Only interrupt routing (gic.c), scheduler timekeeping
       ## (timer.c), and kernel diagnostic serial output (debuglog.c) run in EL1.
   1 | # =========================================================================
   2 | # kernel/dev/Makefile - EL1 GICv2 Interrupt Controller, Timer & Early DebugLog
   3 | # =========================================================================
   4 | ROOT_DIR ?= $(abspath ../..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## Auto-discover gic.c, timer.c, and debuglog.c and map them to gic.o, timer.o,
       ## and debuglog.o.
   7 | C_SRCS := $(wildcard *.c)
   8 | OBJS   := $(C_SRCS:.c=.o)
   9 | 
  10 | .PHONY: all clean
  11 | 
       ## Compile the three EL1 device objects and remove them on clean.
  12 | all: $(OBJS)
  13 | 	@:
  14 | 
  15 | clean:
  16 | 	@rm -f $(OBJS)
```

#### Technical Notes
* **Microkernel vs. Monolithic `kernel/chr_drv` and `kernel/blk_drv`**: Compare `kernel/dev/Makefile` (3 small files totaling ~230 lines of C) with Linux 0.12's `kernel/chr_drv/Makefile` and `kernel/blk_drv/Makefile` in ECLK Chapter 16, which compile thousands of lines of keyboard scancode tables, VT102 escape parsers, TTY ring buffers, and floppy/IDE elevator queues into `EL1`/`Ring 0`. In ABOS, all interactive terminal multiplexing and block DMA live in unprivileged `EL0` processes built under `user/drivers/`.

---

## 16.4 User-Space Subdirectory Makefiles

Under `user/`, ABOS builds five subdirectories that populate `EL0` user space:
1. `user/ulibc/`: The C runtime startup (`crt0.o`, `crt_env.o`), syscall veneer (`ulibc.o`), and static library archive (`libulibc.a`).
2. `user/userboot/`: The root bootstrap object (`userboot.o`) embedded into `kernel.img`.
3. `user/drivers/`: The standalone BOOTFS drivers (`console-srv.bin` and `fs-srv.bin`).
4. `user/shell/`: The standalone BOOTFS capability shell (`shell.bin`).
5. `user/apps/`: Disk-resident user applications (`calc.bin`, plus any `<app>.c` added by the reader).

### 16.4.1 User-Space Subsystem Aggregator: `user/Makefile`

#### Functional Description
`user/Makefile` coordinates all five `EL0` subdirectories while enforcing the critical dependency that `ulibc` (`crt0.o` and `libulibc.a`) must finish building before any standalone executable in `drivers`, `shell`, or `apps` attempts to link.

#### Heavily Commented Source Code

#### Program 16-8: `lesson_20/user/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/user/Makefile" start="1" end="22" total="22" -->
```makefile
   1 | # =========================================================================
   2 | # user/Makefile - EL0 User-Space Subsystems (ulibc, drivers, userboot, shell, apps)
   3 | # =========================================================================
       ## Resolve ROOT_DIR and include shared toolchain settings from config.mk.
   4 | ROOT_DIR ?= $(abspath ..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## Enumerate all five EL0 user-space subdirectories.
   7 | SUBDIRS := ulibc drivers userboot shell apps
   8 | 
   9 | .PHONY: all clean $(SUBDIRS)
  10 | 
  11 | all: $(SUBDIRS)
  12 | 
       ## Critical DAG ordering constraint: 'drivers', 'shell', and 'apps' all link
       ## standalone EL0 ELF binaries against $(CRT0_OBJ) (user/ulibc/crt0.o) and
       ## $(ULIBC_A) (user/ulibc/libulibc.a). Declaring 'drivers shell apps: ulibc'
       ## guarantees that even under 'make -j8', ulibc finishes archiving libulibc.a
       ## before drivers, shell, and apps start linking in parallel!
       ## Notice 'userboot' is NOT constrained to wait for libulibc.a because
       ## user/userboot/Makefile only compiles userboot.c -> userboot.o (it does not
       ## link an ELF executable).
  13 | # Ensure ulibc (crt0.o + libulibc.a) is built before standalone servers/shells/apps
  14 | drivers shell apps: ulibc
  15 | 
       ## Invoke 'make -C $@ all' for each subdirectory and clean all five
       ## subdirectories on 'make clean'.
  16 | $(SUBDIRS):
  17 | 	@$(MAKE) --no-print-directory -C $@ all
  18 | 
  19 | clean:
  20 | 	@for dir in $(SUBDIRS); do \
  21 | 		$(MAKE) --no-print-directory -C $$dir clean; \
  22 | 	done
```

#### Technical Notes
* **Race-free parallel builds**: A classic bug in multi-directory `Makefile` trees occurs when two subdirectories race to build or read the same static library (`libulibc.a`) under `make -j`. Because `user/Makefile` expresses `drivers shell apps: ulibc` as a native dependency edge in GNU `make`'s graph, `ulibc` is built exactly once before `drivers`, `shell`, and `apps` fan out concurrently.

---

### 16.4.2 C Runtime and Static Library: `user/ulibc/Makefile`

#### Functional Description
`user/ulibc/Makefile` compiles the three foundational `EL0` runtime components—`crt0.S` (`_start` entry point and `.bss` zeroing), `crt_env.c` (bootstrap channel `ab_proc_args_t` unpacking and `printf`/`u_gets`), and `ulibc.c` (`svc #0` syscall wrappers, string helpers, and `u_vsnprintf` formatter)—and bundles them into the static archive `libulibc.a`.

#### Heavily Commented Source Code

#### Program 16-9: `lesson_20/user/ulibc/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/user/ulibc/Makefile" start="1" end="20" total="20" -->
```makefile
   1 | # =========================================================================
   2 | # user/ulibc/Makefile - EL0 C Runtime (crt0.o, crt_env.o) & Static Library (libulibc.a)
   3 | # =========================================================================
       ## Resolve ROOT_DIR two levels up and include config.mk.
   4 | ROOT_DIR ?= $(abspath ../..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## Explicitly list the three runtime objects:
       ##   - crt0.o    : Assembly entry _start in section .text._start (placed at 0x20000000 by user/link.ld)
       ##   - crt_env.o : __ulibc_init() startup handle unpacker + stateful printf() / dlog_printf()
       ##   - ulibc.o   : Stateless __ab_syscall4() SVC stubs + u_snprintf / shell_print / string utilities
   7 | OBJS := crt0.o crt_env.o ulibc.o
   8 | LIB  := libulibc.a
   9 | 
  10 | .PHONY: all clean
  11 | 
       ## Default target builds libulibc.a (which transitively compiles crt0.o,
       ## crt_env.o, and ulibc.o, leaving the individual .o files intact because OBJS
       ## are explicit prerequisites rather than intermediate pattern temporaries).
  12 | all: $(LIB)
  13 | 	@:
  14 | 
       ## Archive rule: '$(AR) rcs $@ $^' creates or updates libulibc.a:
       ##   - 'r' : insert/replace member object files in the archive
       ##   - 'c' : create the archive silently if it does not yet exist
       ##   - 's' : write an ELF symbol-table index (equivalent to running 'ranlib') so GNU ld can resolve cross-object references (such as crt_env.o calling sys_channel_read and u_vsnprintf in ulibc.o) in a single pass!
  15 | $(LIB): $(OBJS)
  16 | 	@echo "  [AR]      user/ulibc/$@"
  17 | 	@$(AR) rcs $@ $^
  18 | 
       ## Clean removes both the three .o files and libulibc.a.
  19 | clean:
  20 | 	@rm -f $(OBJS) $(LIB)
```

#### Technical Notes
* **Why `ulibc` splits `crt_env.c` from `ulibc.c`**: `crt_env.c` defines writable static `.bss` arrays (`__ulibc_handles[8]`, `__ulibc_log_h`, `__ulibc_con_ch`), whereas `ulibc.c` is 100% pure, stateless stack-only code. This split allows `image/Makefile` to link `user/ulibc/ulibc.o` directly alongside `userboot.o` inside the read-only kernel image (giving `userboot` full access to `sys_*`, `ulog`, `u_snprintf`, and `u_strcmp` without touching `.bss`), while standalone `EL0` processes link `libulibc.a` and enjoy full `crt_env.o` globals (`printf()`, `u_gets()`, `ulibc_get_startup_handle()`).

---

### 16.4.3 Root Bootstrap Object: `user/userboot/Makefile`

#### Functional Description
`user/userboot/Makefile` compiles `userboot.c` into the relocatable object `userboot.o`, which `image/Makefile` links into `kernel.elf` so `lk_main()` can launch the root `EL0` bootstrap process without needing an in-kernel ELF or TAR parser.

#### Heavily Commented Source Code

#### Program 16-10: `lesson_20/user/userboot/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/user/userboot/Makefile" start="1" end="16" total="16" -->
```makefile
   1 | # =========================================================================
   2 | # user/userboot/Makefile - EL0 Root Bootstrap Service (userboot.c)
   3 | # =========================================================================
       ## Include shared build variables and pattern rules from config.mk.
   4 | ROOT_DIR ?= $(abspath ../..)
   5 | include $(ROOT_DIR)/config.mk
   6 | 
       ## Compile userboot.c -> userboot.o only! Unlike drivers/, shell/, and apps/,
       ## userboot/Makefile does NOT invoke $(LD) or $(OBJCOPY), because userboot.o will
       ## be linked into image/kernel.elf and executed in EL0 via the read-only
       ## 'userboot-rodata' VMO mapping.
   7 | C_SRCS := $(wildcard *.c)
   8 | OBJS   := $(C_SRCS:.c=.o)
   9 | 
  10 | .PHONY: all clean
  11 | 
       ## Ensure userboot.o is up-to-date on 'all' and deleted on 'clean'.
  12 | all: $(OBJS)
  13 | 	@:
  14 | 
  15 | clean:
  16 | 	@rm -f $(OBJS)
```

#### Technical Notes
* **How `userboot.o` bridges `EL1` virtual addresses and `EL0` virtual addresses**: When `image/Makefile` links `userboot.o` into `kernel.elf` via `linker.ld`, `userboot_entry` receives a higher-half kernel virtual address (`0xFFFFFFFF80094d70`). Because `-fno-pic -fno-pie` emits PC-relative `adrp`/`add` and `bl` instructions (whose relative offsets are invariant under a constant translation offset!), `lk_main()` maps the kernel's `.text`/`.rodata` pages read+execute into `userboot`'s `TTBR0_EL1` address space at `kva - 0xFFFFFFFF80000000` (`0x00094d70`) via `USER_KVA_TO_UVA(userboot_entry)` and drops to `EL0` seamlessly.

---

### 16.4.4 Standalone EL0 System Drivers: `user/drivers/Makefile`

#### Functional Description
`user/drivers/Makefile` compiles and links ABOS's two privileged-capability user-space hardware drivers—`console_srv.c` (PL011 UART RX IRQ 33 + 4-VT multiplexer) and `fs_srv.c` (VirtIO-Blk MMIO/DMA + USTAR filesystem server)—into standalone `EL0` ELF executables (`image/console-srv.elf`, `image/fs-srv.elf`) and flat loadable binaries (`image/console-srv.bin`, `image/fs-srv.bin`) linked at virtual address `0x20000000` via `user/link.ld`.

#### Heavily Commented Source Code

#### Program 16-11: `lesson_20/user/drivers/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/user/drivers/Makefile" start="1" end="36" total="36" -->
```makefile
   1 | # =========================================================================
   2 | # user/drivers/Makefile - Standalone EL0 User-Space Drivers
   3 | #
   4 | # Links console-srv.bin and fs-srv.bin as standalone EL0 executables at
   5 | # VA 0x20000000 using the common linker script $(USER_LD) (user/link.ld).
   6 | # =========================================================================
       ## Resolve ROOT_DIR and include config.mk.
   7 | ROOT_DIR ?= $(abspath ../..)
   8 | include $(ROOT_DIR)/config.mk
   9 | 
       ## Define local driver object files (console_srv.o, fs_srv.o) and output
       ## artifacts placed directly into $(IMAGE_DIR)/ (note the hyphenated binary names
       ## 'console-srv.bin' and 'fs-srv.bin' expected by userboot.c and make_bootfs.py).
  10 | OBJS     := console_srv.o fs_srv.o
  11 | SRV_ELFS := $(IMAGE_DIR)/console-srv.elf $(IMAGE_DIR)/fs-srv.elf
  12 | SRV_BINS := $(IMAGE_DIR)/console-srv.bin $(IMAGE_DIR)/fs-srv.bin
  13 | 
       ## '.PRECIOUS: $(OBJS) $(SRV_ELFS)' is critical for GDB debugging! Without
       ## .PRECIOUS, GNU make treats console-srv.elf and fs-srv.elf as intermediate
       ## chain files on the way to %.bin and automatically deletes them at the end of
       ## the build. Marking them .PRECIOUS preserves image/console-srv.elf and
       ## image/fs-srv.elf with full DWARF debug info so 'add-symbol-file
       ## image/console-srv.elf 0x20000000' works in GDB!
  14 | .PRECIOUS: $(OBJS) $(SRV_ELFS)
  15 | .PHONY: all clean
  16 | 
  17 | all: $(SRV_BINS)
  18 | 	@:
  19 | 
       ## Self-contained convenience rule: if a developer runs 'cd user/drivers && make'
       ## directly after 'make clean', automatically trigger user/ulibc/Makefile to
       ## build crt0.o and libulibc.a.
  20 | $(ULIBC_A) $(CRT0_OBJ):
  21 | 	@$(MAKE) --no-print-directory -C $(ULIBC_DIR) all
  22 | 
       ## Link rules for console-srv.elf (lines 23-25) and fs-srv.elf (lines 27-29)
       ## using $(USER_LD) (user/link.ld at VA 0x20000000).
       ## Pay close attention to the argument order passed to $(LD): '$(CRT0_OBJ) $<
       ## $(ULIBC_A)':
       ##   1. $(CRT0_OBJ) is passed as an explicit object file so _start (.text._start) is unconditionally pulled in as ENTRY(_start) at 0x20000000.
       ##   2. $< (console_srv.o or fs_srv.o) follows, defining main() and leaving unresolved symbol references to ulibc functions.
       ##   3. $(ULIBC_A) comes LAST because traditional Unix/GNU ld scans static '.a' archives left-to-right once and extracts only archive members that satisfy currently unresolved symbols!
  23 | $(IMAGE_DIR)/console-srv.elf: console_srv.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
  24 | 	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
  25 | 	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)
  26 | 
  27 | $(IMAGE_DIR)/fs-srv.elf: fs_srv.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
  28 | 	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
  29 | 	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)
  30 | 
       ## Pattern rule converting each linked ELF executable into a flat raw memory
       ## image ('objcopy -O binary'): strips ELF headers, section tables, and DWARF
       ## sections, emitting raw bytes starting at 0x20000000 (.text._start -> .text ->
       ## .rodata -> .data) ready to be mapped directly into a VMO by userboot!
  31 | $(IMAGE_DIR)/%.bin: $(IMAGE_DIR)/%.elf
  32 | 	@echo "  [OBJCOPY] image/$(notdir $@)"
  33 | 	@$(OBJCOPY) -O binary $< $@
  34 | 
       ## Clean removes local .o files as well as the generated .elf and .bin files in
       ## image/.
  35 | clean:
  36 | 	@rm -f $(OBJS) $(SRV_ELFS) $(SRV_BINS)
```

#### Technical Notes
* **Why `.bss` occupies zero bytes in `.bin` yet works at runtime**: When `objcopy -O binary` converts `console-srv.elf` (`46,688` bytes with DWARF info) into `console-srv.bin` (`15,508` bytes), `NOBITS` sections normally at the very end of an image are omitted unless followed by a `PROGBITS` section. Look back at `user/link.ld` lines 45–47:
  ```ld
  .app_end ALIGN(8) : {
      LONG(0x0)
  }
  ```
  Because `.app_end` emits a 4-byte `PROGBITS` word *after* `.bss`, `objcopy -O binary` materializes the exact byte span of `.bss` inside the `.bin` file size! Consequently, when `userboot.c` computes `map_sz = ROUND_UP(fsz, PAGE_SIZE)` and creates `bin_vmo`, the VMO automatically has enough pages mapped to cover `.text`, `.rodata`, `.data`, *and* `.bss` without needing to parse ELF program headers (`p_memsz` vs `p_filesz`)! And when `_start` in `crt0.S` runs, it zeroes `[__bss_start .. __bss_end)` cleanly before calling `main()`.

---

### 16.4.5 Standalone Interactive Shell: `user/shell/Makefile`

#### Functional Description
`user/shell/Makefile` compiles `user/shell/shell.c` and links it against `crt0.o` and `libulibc.a` via `user/link.ld` to produce `image/shell.elf` (with DWARF symbols) and `image/shell.bin` (packed into `bootfs.tar` and spawned four times as `abos-shell-0` .. `abos-shell-3`).

#### Heavily Commented Source Code

#### Program 16-12: `lesson_20/user/shell/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/user/shell/Makefile" start="1" end="32" total="32" -->
```makefile
   1 | # =========================================================================
   2 | # user/shell/Makefile - Standalone EL0 Interactive Capability Shell
   3 | #
   4 | # Links shell.bin as a standalone EL0 executable at VA 0x20000000 using
   5 | # the common linker script $(USER_LD) (user/link.ld).
   6 | # =========================================================================
       ## Include shared configuration from lesson_20/config.mk.
   7 | ROOT_DIR ?= $(abspath ../..)
   8 | include $(ROOT_DIR)/config.mk
   9 | 
       ## Define shell.o and its target outputs image/shell.elf and image/shell.bin.
  10 | OBJS      := shell.o
  11 | SHELL_ELF := $(IMAGE_DIR)/shell.elf
  12 | SHELL_BIN := $(IMAGE_DIR)/shell.bin
  13 | 
       ## Preserve shell.o and image/shell.elf (.PRECIOUS) so source-level GDB debugging
       ## ('add-symbol-file image/shell.elf 0x20000000') has full function, variable,
       ## and line-number metadata.
  14 | .PRECIOUS: $(OBJS) $(SHELL_ELF)
  15 | .PHONY: all clean
  16 | 
  17 | all: $(SHELL_BIN)
  18 | 	@:
  19 | 
       ## Ensure ulibc artifacts exist if make is invoked directly inside user/shell/.
  20 | $(ULIBC_A) $(CRT0_OBJ):
  21 | 	@$(MAKE) --no-print-directory -C $(ULIBC_DIR) all
  22 | 
       ## Link shell.elf at VA 0x20000000 with user/link.ld (identical linker script and
       ## link recipe to console-srv.elf and fs-srv.elf!) and extract flat binary
       ## image/shell.bin via objcopy.
  23 | $(SHELL_ELF): shell.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
  24 | 	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
  25 | 	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)
  26 | 
  27 | $(SHELL_BIN): $(SHELL_ELF)
  28 | 	@echo "  [OBJCOPY] image/$(notdir $@)"
  29 | 	@$(OBJCOPY) -O binary $< $@
  30 | 
       ## Clean removes shell.o, image/shell.elf, and image/shell.bin.
  31 | clean:
  32 | 	@rm -f $(OBJS) $(SHELL_ELF) $(SHELL_BIN)
```

#### Technical Notes
* **One binary, four isolated address spaces**: Even though `user/shell/Makefile` produces only a single `20,516`-byte `image/shell.bin`, `userboot` spawns four independent processes (`abos-shell-0` through `abos-shell-3`, ASIDs `4..7`) from it. Each shell instance executes `main()` at the identical virtual address `0x20000a68` while holding its own private VMO copy, private 24 KB `user-stack` at `0x00800000`, and private `con_cli_end[i]` / `fs_cli_end[i]` IPC channel endpoints.

---

### 16.4.6 Auto-Discovered User Applications: `user/apps/Makefile`

#### Functional Description
`user/apps/Makefile` implements a zero-configuration build pipeline for disk-resident `EL0` user applications. Any `<app>.c` source file placed in `user/apps/` (such as `calc.c`) is automatically discovered via `$(wildcard *.c)`, compiled, linked at `0x20000000` with `user/link.ld`, stripped into `image/<app>.bin`, and subsequently picked up automatically by `image/make_tar_disk.py` into `disk.img`.

#### Heavily Commented Source Code

#### Program 16-13: `lesson_20/user/apps/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/user/apps/Makefile" start="1" end="37" total="37" -->
```makefile
       ## Header comment highlighting the zero-configuration workflow: dropping a new C
       ## file (e.g., hello.c or crash.c in Section 17.6) into user/apps/ requires zero
       ## Makefile edits.
   1 | # =========================================================================
   2 | # user/apps/Makefile - Standalone EL0 User-Space Applications
   3 | #
   4 | # All user-space applications share the common linker script $(USER_LD)
   5 | # (user/link.ld), C runtime startup $(CRT0_OBJ), and library $(ULIBC_A).
   6 | # Adding any new <app>.c in user/apps/ automatically builds:
   7 | #   - $(IMAGE_DIR)/<app>.elf
   8 | #   - $(IMAGE_DIR)/<app>.bin
   9 | # =========================================================================
  10 | ROOT_DIR ?= $(abspath ../..)
  11 | include $(ROOT_DIR)/config.mk
  12 | 
       ## Dynamic target generation pipeline using GNU make text functions:
       ##   - APP_SRCS  : e.g., 'calc.c' (or 'calc.c hello.c')
       ##   - APP_OBJS  : 'calc.o'
       ##   - APP_NAMES : 'calc' ($(basename ...) strips the .c extension)
       ##   - APP_ELFS  : '$(IMAGE_DIR)/calc.elf' (for GDB symbol loading)
       ##   - APP_BINS  : '$(IMAGE_DIR)/calc.bin' (for TAR disk packaging)
  13 | APP_SRCS := $(wildcard *.c)
  14 | APP_OBJS := $(APP_SRCS:.c=.o)
  15 | APP_NAMES := $(basename $(APP_SRCS))
  16 | APP_ELFS := $(addprefix $(IMAGE_DIR)/,$(addsuffix .elf,$(APP_NAMES)))
  17 | APP_BINS := $(addprefix $(IMAGE_DIR)/,$(addsuffix .bin,$(APP_NAMES)))
  18 | 
       ## Mark $(APP_OBJS) and $(APP_ELFS) as .PRECIOUS so GNU make does not delete
       ## image/<app>.elf as an intermediate file after building image/<app>.bin.
  19 | .PRECIOUS: $(APP_OBJS) $(APP_ELFS)
  20 | .PHONY: all clean
  21 | 
  22 | all: $(APP_BINS)
  23 | 	@:
  24 | 
       ## Ensure ulibc is built if 'make' is run directly inside user/apps/.
  25 | $(ULIBC_A) $(CRT0_OBJ):
  26 | 	@$(MAKE) --no-print-directory -C $(ULIBC_DIR) all
  27 | 
       ## Generic pattern rules linking any %.o in user/apps/ into $(IMAGE_DIR)/%.elf at
       ## VA 0x20000000 using $(CRT0_OBJ), $(ULIBC_A), and $(USER_LD), then stripping it
       ## to $(IMAGE_DIR)/%.bin via objcopy.
  28 | $(IMAGE_DIR)/%.elf: %.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
  29 | 	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
  30 | 	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)
  31 | 
  32 | $(IMAGE_DIR)/%.bin: $(IMAGE_DIR)/%.elf
  33 | 	@echo "  [OBJCOPY] image/$(notdir $@)"
  34 | 	@$(OBJCOPY) -O binary $< $@
  35 | 
       ## Clean deletes all discovered app .o, .elf, and .bin artifacts.
  36 | clean:
  37 | 	@rm -f $(APP_OBJS) $(APP_ELFS) $(APP_BINS)
```

#### Technical Notes
* **Uniform `EL0` execution contract**: Compare lines 28–34 of `user/apps/Makefile` with `user/drivers/Makefile` and `user/shell/Makefile`. All three use the **exact same linker script (`user/link.ld`), startup object (`crt0.o`), and static library (`libulibc.a`)**. In a capability microkernel, the *only* difference between a privileged hardware driver (`fs-srv`) and an untrusted calculator app (`calc.bin`) is not how it is compiled or linked, but **which capability handles its parent transfers into its bootstrap channel** (`AB_PA_RESOURCE` vs. only `AB_PA_LOG` + `AB_PA_CONSOLE`)!

---

## 16.5 Final Image Packaging and USTAR Archive Builders

Once `kernel/` and `user/` have compiled all `EL1` object files and standalone `EL0` binaries, `image/Makefile` and its two companion Python scripts (`make_bootfs.py` and `make_tar_disk.py`) assemble the three bootable images consumed by QEMU.

### 16.5.1 Image Linker and Packager Rules: `image/Makefile`

#### Functional Description
`image/Makefile` performs the final assembly of the operating system:
1. Links the `EL1` microkernel object files (`kernel/core/*.o`, `kernel/mm/*.o`, `kernel/dev/*.o`) together with *only* `user/ulibc/ulibc.o` and `user/userboot/userboot.o` using `linker.ld` to produce `image/kernel.elf`, then strips it via `objcopy -O binary` into `image/kernel.img`.
2. Invokes `make_bootfs.py` whenever `console-srv.bin`, `fs-srv.bin`, `shell.bin`, or the script itself changes, producing `image/bootfs.tar`.
3. Invokes `make_tar_disk.py` whenever any `user/apps/*.bin` or the script itself changes, producing the 1 MB `image/disk.img`.

#### Heavily Commented Source Code

#### Program 16-14: `lesson_20/image/Makefile`

<!-- ECLK_CODE_LISTING file="lesson_20/image/Makefile" start="1" end="59" total="59" -->
```makefile
       ## Architectural summary emphasizing the three-image microkernel separation:
       ## kernel.img (-kernel), bootfs.tar (-initrd), and disk.img (-drive).
   1 | # =========================================================================
   2 | # image/Makefile - Final Image Packaging (kernel.img, bootfs.tar, disk.img)
   3 | #
   4 | # Notice that console-srv, fs-srv, and shell are completely separate from
   5 | # kernel.elf:
   6 | #   1. kernel.elf links ONLY EL1 kernel + userboot (passed to QEMU -kernel)
   7 | #   2. bootfs.tar packs console-srv.bin, fs-srv.bin, and shell.bin (passed to QEMU -initrd)
   8 | #   3. disk.img packs motd.txt, zircon.txt, and standalone apps (passed to QEMU -drive)
   9 | # =========================================================================
  10 | ROOT_DIR ?= $(abspath ..)
  11 | include $(ROOT_DIR)/config.mk
  12 | 
       ## Collect all EL1 C sources across kernel/{core,mm,dev} and assembly sources in
       ## kernel/core/.
  13 | KERNEL_C_SRCS := $(wildcard $(ROOT_DIR)/kernel/core/*.c) \
  14 |                  $(wildcard $(ROOT_DIR)/kernel/mm/*.c) \
  15 |                  $(wildcard $(ROOT_DIR)/kernel/dev/*.c)
  16 | KERNEL_S_SRCS := $(wildcard $(ROOT_DIR)/kernel/core/*.S)
  17 | 
       ## Crucial microkernel boundary: USERBOOT_OBJS includes ONLY ulibc.o (stateless
       ## syscall stubs) and userboot.o! Neither crt0.o, crt_env.o, console_srv.o,
       ## fs_srv.o, shell.o, nor calc.o is linked into kernel.elf.
  18 | # Only userboot and ulibc syscall stubs are linked into the kernel image
  19 | USERBOOT_OBJS := $(ROOT_DIR)/user/ulibc/ulibc.o \
  20 |                  $(ROOT_DIR)/user/userboot/userboot.o
  21 | 
       ## Combine all 17 EL1 kernel objects + 2 userboot objects = 19 total relocatable
       ## objects in $(SYS_OBJS). Note that section placement (.idmap at PA 0x40080000
       ## vs .text at VA 0xFFFFFFFF80084000) is governed by section names in linker.ld
       ## rather than object file command-line order.
  22 | SYS_OBJS      := $(KERNEL_C_SRCS:.c=.o) \
  23 |                  $(USERBOOT_OBJS) \
  24 |                  $(KERNEL_S_SRCS:.S=.o)
  25 | 
       ## Declare the three BOOTFS server binaries (already staged in image/ by
       ## user/drivers/Makefile and user/shell/Makefile) and dynamically compute
       ## APP_BINS from user/apps/*.c so adding a new app triggers a rebuild of
       ## disk.img.
  26 | BOOTFS_BINS   := console-srv.bin fs-srv.bin shell.bin
  27 | BOOTFS_TAR    := bootfs.tar
  28 | 
  29 | APP_SRCS      := $(wildcard $(ROOT_DIR)/user/apps/*.c)
  30 | APP_NAMES     := $(basename $(notdir $(APP_SRCS)))
  31 | APP_BINS      := $(addsuffix .bin,$(APP_NAMES))
  32 | 
  33 | KERNEL_ELF    := kernel.elf
  34 | KERNEL_IMG    := kernel.img
  35 | DISK_IMG      := disk.img
  36 | 
  37 | .PHONY: all clean
  38 | 
       ## Default 'all' target builds all three final images: kernel.img, bootfs.tar,
       ## and disk.img.
  39 | all: $(KERNEL_IMG) $(BOOTFS_TAR) $(DISK_IMG)
  40 | 	@:
  41 | 
       ## Link image/kernel.elf with '-n -T linker.ld'. Keeping kernel.elf on disk
       ## alongside kernel.img is essential: QEMU boots the raw binary kernel.img, while
       ## gdb-multiarch loads symbols and DWARF line tables from kernel.elf!
  42 | $(KERNEL_ELF): $(SYS_OBJS) $(KERNEL_LD)
  43 | 	@echo "  [LD]      image/$@ (EL1 kernel + userboot only)"
  44 | 	@$(LD) $(KERNEL_LDFLAGS) -o $@ $(SYS_OBJS)
  45 | 
       ## Convert kernel.elf (244 KB with DWARF info) into flat binary kernel.img
       ## (102,400 bytes = 25 4KB pages). Because linker.ld uses 'AT(ADDR(.idmap) +
       ## SIZEOF(.idmap))' on line 28 to set the Load Memory Address (LMA) of .text
       ## contiguous with .idmap (PA 0x40084000) even though its Virtual Memory Address
       ## (VMA) jumps to 0xFFFFFFFF80084000, 'objcopy -O binary' emits a compact 100 KB
       ## file instead of trying to zero-pad a multi-exabyte gap!
  46 | $(KERNEL_IMG): $(KERNEL_ELF)
  47 | 	@echo "  [OBJCOPY] image/$@"
  48 | 	@$(OBJCOPY) -O binary $< $@
  49 | 
       ## Build bootfs.tar via make_bootfs.py whenever any BOOTFS binary or the script
       ## changes.
  50 | $(BOOTFS_TAR): $(BOOTFS_BINS) make_bootfs.py
  51 | 	@echo "  [BOOTFS]  image/$@ ($(BOOTFS_BINS))"
  52 | 	@python3 make_bootfs.py
  53 | 
       ## Build 1 MB disk.img via make_tar_disk.py whenever any user/apps/*.bin or the
       ## script changes.
  54 | $(DISK_IMG): $(APP_BINS) make_tar_disk.py
  55 | 	@echo "  [TAR]     image/$@ (motd.txt, zircon.txt, $(APP_BINS))"
  56 | 	@python3 make_tar_disk.py
  57 | 
       ## Clean removes all generated archives, kernel images, and staged .elf/.bin/.o
       ## files in image/.
  58 | clean:
  59 | 	@rm -f $(BOOTFS_TAR) $(KERNEL_ELF) $(KERNEL_IMG) $(DISK_IMG) *.elf *.bin *.o
```

#### Technical Notes
* **Why `AT(...)` in `linker.ld` saves `objcopy -O binary` from a 16-exabyte file**: `objcopy -O binary` lays out sections in the output file according to each section's **LMA (Load Memory Address)**, not its **VMA (Virtual Memory Address)**. In `linker.ld`:
  * `.idmap` has `VMA = LMA = 0x40080000` (size `0x4000` = 16 KB: 4 KB code + `mmu_tables_ready` + 4 KB `idmap_table` + 4 KB `kernel_table`).
  * `.text` has `VMA = 0xFFFFFFFF80084000`, but `AT(ADDR(.idmap) + SIZEOF(.idmap))` sets its `LMA = 0x40084000` (and subsequent `.rodata`, `.data` inherit contiguous LMAs).
  Therefore, `objcopy -O binary` writes a contiguous `102,400`-byte `kernel.img` loaded by QEMU at `0x40080000`. Once `start.S` enables `TTBR1_EL1` (mapping `0xFFFFFFFF80000000 -> 0x40000000`), physical address `0x40084000` simultaneously appears at its linked higher-half VMA `0xFFFFFFFF80084000`!

---

### 16.5.2 BOOTFS Initrd Builder: `image/make_bootfs.py`

#### Functional Description
`image/make_bootfs.py` constructs `image/bootfs.tar`, an uncompressed POSIX.1-1988 **USTAR** (`Unix Standard TAR`) archive containing `console-srv.bin`, `fs-srv.bin`, and `shell.bin`. Passed to QEMU via `-initrd image/bootfs.tar`, this archive is placed in guest DRAM by QEMU, wrapped into a read-only `bootfs.tar` VMO by `lk_main()`, and parsed in `EL0` by `userboot.c:bootfs_find_file()`.

#### Heavily Commented Source Code

#### Program 16-15: `lesson_20/image/make_bootfs.py`

<!-- ECLK_CODE_LISTING file="lesson_20/image/make_bootfs.py" start="1" end="19" total="19" -->
```python
       ## Use Python 3's standard-library io, os, and tarfile modules—zero third-party
       ## pip packages or host GNU/BSD tar CLI flag differences (such as macOS bsdtar
       ## inserting AppleDouble '._*' extended-attribute headers that can confuse
       ## minimal OS parsers!).
   1 | import io, os, tarfile
   2 | 
       ## Resolve paths relative to __file__ so make_bootfs.py works reliably regardless
       ## of the caller's current working directory.
   3 | script_dir = os.path.dirname(os.path.abspath(__file__))
   4 | bootfs_path = os.path.join(script_dir, "bootfs.tar")
   5 | 
       ## Fixed manifest of the three essential user-space system servers required at
       ## boot: console-srv.bin, fs-srv.bin, and shell.bin.
   6 | boot_bins = ["console-srv.bin", "fs-srv.bin", "shell.bin"]
   7 | 
       ## Assemble the archive in an in-memory BytesIO buffer using explicit
       ## 'tarfile.USTAR_FORMAT' (classic 512-byte POSIX USTAR header blocks without GNU
       ## longname extensions or PAX extended headers).
   8 | buf = io.BytesIO()
   9 | with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
       ## For each binary, read its raw bytes and construct a fresh TarInfo(name=name)
       ## metadata record with ONLY 'name' and 'size' populated (uid=0, gid=0, mtime=0 /
       ## 1970-01-01 00:00:00, mode=0644). This guarantees 100% reproducible,
       ## byte-for-byte deterministic builds and never leaks the developer's host
       ## username or UID into the OS image!
  10 |     for name in boot_bins:
  11 |         full_path = os.path.join(script_dir, name)
  12 |         with open(full_path, "rb") as bf:
  13 |             data = bf.read()
  14 |         ti = tarfile.TarInfo(name=name)
  15 |         ti.size = len(data)
  16 |         tf.addfile(ti, io.BytesIO(data))
  17 | 
       ## Atomically write the finished USTAR byte stream (61,440 bytes = 15 4KB pages,
       ## including 512-byte record padding and two trailing 512-byte all-zero
       ## end-of-archive blocks) to image/bootfs.tar.
  18 | with open(bootfs_path, "wb") as f:
  19 |     f.write(buf.getvalue())
```

#### Technical Notes
* **POSIX USTAR 512-byte binary layout and how `userboot` & `fs-srv` parse it**: Every entry in `bootfs.tar` (and `disk.img`) consists of a 512-byte header block followed by `ceil(size / 512) * 512` data bytes, terminated at the end of the archive by two 512-byte zero blocks (`hdr[0] == '\0'`):
  ```text
  Offset   Length   Field       Encoding in USTAR           Consumer in ABOS
  ------   ------   -----       -----------------           ----------------
  0        100      name        NUL-terminated ASCII        u_strcmp(hdr, name)
  100      8        mode        Octal ASCII ("0000644\0")   (informational)
  108      8        uid         Octal ASCII ("0000000\0")   0 (deterministic)
  116      8        gid         Octal ASCII ("0000000\0")   0 (deterministic)
  124      12       size        Octal ASCII + NUL           parse_octal(hdr + 124, 12)
  136      12       mtime       Octal ASCII ("00000000000") 0 (1970-01-01)
  148      8        chksum      Header byte sum in octal    Verified by host tar -tvf
  156      1        typeflag    '0' or '\0' (regular file)  Regular file
  257      6        magic       "ustar\0"                   USTAR_FORMAT signature
  512      fsz      payload     Raw .bin or .txt bytes      Copied into VMO!
  ```
  Because USTAR aligns every file header and file payload to a 512-byte sector boundary and terminates with zeroed blocks, `userboot.c:bootfs_find_file()` needs only **15 lines of C** (`off += 512 + ((fsz + 511U) & ~511U)`) to locate any BOOTFS executable!

---

### 16.5.3 VirtIO-Blk TAR Disk Builder: `image/make_tar_disk.py`

#### Functional Description
`image/make_tar_disk.py` builds the 1 MB (`1,048,576`-byte = `2,048` 512-byte sectors) raw block-device image `image/disk.img` attached to QEMU's VirtIO-Blk controller. It embeds two welcome/documentation text files (`motd.txt` and `zircon.txt`), automatically scans `image/` to pack every non-BOOTFS `*.bin` user application (such as `calc.bin`), verifies that the archive fits within 1 MB, and zero-pads the image to an exact 1 MB disk geometry.

#### Heavily Commented Source Code

#### Program 16-16: `lesson_20/image/make_tar_disk.py`

<!-- ECLK_CODE_LISTING file="lesson_20/image/make_tar_disk.py" start="1" end="39" total="39" -->
```python
       ## Standard-library imports and script-relative output path for image/disk.img.
   1 | import io, os, tarfile
   2 | 
   3 | script_dir = os.path.dirname(os.path.abspath(__file__))
   4 | disk_path = os.path.join(script_dir, "disk.img")
   5 | 
       ## Exclusion set ('boot_only'): image/ holds ALL staged .bin files—both the three
       ## BOOTFS servers and user-space apps. Filtering out boot_only ensures
       ## console-srv.bin, fs-srv.bin, and shell.bin stay exclusively in bootfs.tar and
       ## are not redundantly duplicated onto disk.img.
   6 | boot_only = {"console-srv.bin", "fs-srv.bin", "shell.bin"}
   7 | 
       ## Seed the in-memory file dictionary with two UTF-8 text files ('motd.txt' and
       ## 'zircon.txt') so the interactive shell's 'ls' and zero-copy VMO 'cat <file>'
       ## commands have immediate text files to inspect on boot.
   8 | files = {
   9 |     "motd.txt": (
  10 |         "Welcome to ABOS Lesson 20 (Zircon-Style BOOTFS & Standalone EL0 System Servers)!\n"
  11 |         "console-srv.bin, fs-srv.bin, shell.bin, and calc.bin are all linked with user/link.ld!\n"
  12 |     ).encode("utf-8"),
  13 |     "zircon.txt": (
  14 |         "Zircon Microkernel Principles Demonstrated in ABOS Lesson 20:\n"
  15 |         "  1. Only 'userboot' is embedded in the kernel; servers live in BOOTFS VMO.\n"
  16 |         "  2. console-srv.bin, fs-srv.bin, shell.bin, and calc.bin all share user/link.ld @ 0x20000000.\n"
  17 |         "  3. Run 'vmaps all' to inspect each process's standalone VMO mapping!\n"
  18 |     ).encode("utf-8"),
  19 | }
  20 | 
       ## Auto-discovery loop: iterate over sorted(os.listdir(script_dir)) (sorted for
       ## deterministic archive ordering!) and include every '*.bin' executable not in
       ## boot_only. When a student creates user/apps/hello.c, user/apps/Makefile drops
       ## hello.bin into image/, and this loop automatically packs hello.bin into
       ## disk.img!
  21 | # Automatically include all user-space app *.bin executables in image/
  22 | for fname in sorted(os.listdir(script_dir)):
  23 |     if fname.endswith(".bin") and fname not in boot_only:
  24 |         with open(os.path.join(script_dir, fname), "rb") as bf:
  25 |             files[fname] = bf.read()
  26 | 
       ## Serialize all entries into an in-memory POSIX USTAR archive with deterministic
       ## zeroed TarInfo metadata.
  27 | buf = io.BytesIO()
  28 | with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
  29 |     for name, data in files.items():
  30 |         ti = tarfile.TarInfo(name=name)
  31 |         ti.size = len(data)
  32 |         tf.addfile(ti, io.BytesIO(data))
  33 | 
       ## Enforce raw block-device geometry: VirtIO-Blk exposes disks in 512-byte
       ## sectors, and QEMU's raw drive backend determines disk capacity directly from
       ## host file size. Verify the TAR payload fits inside 1 MB (1,048,576 bytes =
       ## 2,048 sectors), and pad the remainder with zero bytes ('\x00').
       ## Note how this pairs cleanly with fs_srv.c: fs-srv allocates a 32-page (128 KB)
       ## contiguous DMA VMO—3 pages for Virtqueue descriptors/headers and 29 pages (116
       ## KB = 232 sectors) for its TAR RAM cache—which easily holds motd.txt,
       ## zircon.txt, calc.bin, and several additional lab binaries!
  34 | raw = buf.getvalue()
  35 | disk_size = 1024 * 1024
  36 | if len(raw) > disk_size:
  37 |     raise ValueError(f"TAR archive ({len(raw)} bytes) exceeds disk_size ({disk_size} bytes)")
  38 | with open(disk_path, "wb") as f:
  39 |     f.write(raw + b"\x00" * (disk_size - len(raw)))
```

#### Technical Notes
* **Why zero-padding a USTAR archive produces a valid raw disk image**: Because the USTAR specification defines end-of-archive as two or more consecutive 512-byte zero blocks (`hdr->name[0] == '\0'`), appending `(disk_size - len(raw))` zero bytes to `buf.getvalue()` simultaneously satisfies VirtIO-Blk's 1 MB raw sector geometry *and* remains a 100% standards-compliant `.tar` archive that can be inspected directly on the host with `tar -tvf image/disk.img`!

---

## 16.6 Summary

In this chapter, we traced every line of ABOS Lesson 20's 16 build, configuration, and packaging scripts:
1. **`install_tools.sh` and `config.mk`** establish a portable, reproducible AArch64 freestanding build environment (`-ffreestanding -nostdlib -mgeneral-regs-only -fno-pie -fno-pic -g`) across Linux, macOS, and Zircon prebuilt toolchains.
2. **The `Makefile` hierarchy (`Makefile`, `kernel/**/Makefile`, `user/**/Makefile`)** cleanly separates `EL1` microkernel compilation (`core`, `mm`, `dev`), `ulibc` archive construction (`crt0.o` + `libulibc.a`), embedded `userboot.o` compilation, and uniform `VA 0x20000000` `EL0` linkage (`user/link.ld`) while preserving `.elf` DWARF symbols via `.PRECIOUS`.
3. **`image/Makefile`, `make_bootfs.py`, and `make_tar_disk.py`** package the finished system into three capability-aligned artifacts: `kernel.img` (`EL1` + `userboot` only), `bootfs.tar` (standalone `console-srv.bin`, `fs-srv.bin`, `shell.bin`), and `disk.img` (1 MB VirtIO-Blk USTAR image with `motd.txt`, `zircon.txt`, and auto-discovered `user/apps/*.bin` executables).

In **Chapter 17**, we will boot these three artifacts on a 4-core SMP QEMU `virt` machine, dissect every line of the live boot transcript and interactive `caps`/`ps`/`vmaps` diagnostics, attach `gdb-multiarch` across `EL1` and `EL0`, and build three hands-on kernel and user-space extensions.
