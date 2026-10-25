# Toolchains & QEMU Environment for ABOS

## 1. Prebuilt Toolchains in `zircon_org` (Ready Out-of-the-Box!)

Because we have `/usr/local/google/home/lipingliu/80_Zircon/zircon_org` on this machine, its `prebuilt/downloads/` directory already includes a complete, self-contained AArch64 bare-metal toolchain and QEMU emulator:

* **GCC Cross-Compiler (`aarch64-elf-`)**:
  `/usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads/gcc/bin/aarch64-elf-gcc`
* **Clang/LLVM Toolchain**:
  `/usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads/clang/bin/clang`
* **QEMU AArch64 System Emulator**:
  `/usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads/qemu/bin/qemu-system-aarch64`

## 2. Automatic Toolchain Detection in ABOS Makefiles

Every lesson `Makefile` in ABOS automatically checks if `aarch64-linux-gnu-gcc` or `aarch64-elf-gcc` is in your `PATH`, and falls back to the prebuilt toolchain and QEMU inside `/usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads/` so you can run `make` and `make run` directly on the host without needing Docker!

```makefile
ZIRCON_PREBUILT := /usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads

ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-linux-gnu-
else ifneq ($(shell which aarch64-elf-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-elf-
else
    CROSS_COMPILE ?= $(ZIRCON_PREBUILT)/gcc/bin/aarch64-elf-
endif

ifneq ($(shell which qemu-system-aarch64 2>/dev/null),)
    QEMU ?= qemu-system-aarch64
else
    QEMU ?= $(ZIRCON_PREBUILT)/qemu/bin/qemu-system-aarch64
endif
```

## 3. Essential Compiler Flags for AArch64 Microkernel Development

* `-ffreestanding -nostdlib -nostartfiles -fno-builtin`: Do not link against host C libraries or startup files.
* `-mgeneral-regs-only`: Prevents the compiler from emitting NEON/FP instructions (`q0`–`q31`) inside the kernel (matching both `AAOS/lesson_00/issues.md` and Zircon's `kernel/arch/arm64/rules.mk`), avoiding accidental corruption of user-space floating-point registers during syscalls and IRQs.
* `-fno-pie -fno-pic`: Generates position-dependent code suitable for our kernel linker script (`linker.ld`).
