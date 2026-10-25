# ABOS Lesson 01: Bare-Metal Boot, EL3/EL2 $\rightarrow$ EL1 Transition & SMP `lk_main`

## Overview
This lesson combines AAOS's Lesson 01 and Lesson 02b while introducing Zircon's LK-layer boot architecture (`zircon_org/kernel/arch/arm64/start.S`, `zircon_org/kernel/arch/arm64/spinlock.cpp`, and `zircon_org/kernel/top/main.cpp`):

1. **Bootloader Handoff & Exception Level Transition (`start.S`)**:
   * On CPU 0, captures `x0` (the physical address of the Device Tree Blob / ZBI placed by QEMU at `0x40000000`) into `boot_dtb_paddr`, and records the initial `CurrentEL` in `arch_boot_el` (just like `zbi_paddr` and `arch_boot_el` in `zircon_org/kernel/arch/arm64/start.S`).
   * Safely drops from **EL3** (Secure Monitor) and/or **EL2** (Hypervisor) into **EL1h** (Kernel mode), configuring `SCR_EL3`, `HCR_EL2`, `CNTHCTL_EL2` (allowing EL1 timer access), and `CPACR_EL1`.
   * Provides an EL2/EL3 `HVC` $\rightarrow$ `SMC` PSCI shim so secondary core wakeup works whether QEMU is launched in EL3 (`secure=on,virtualization=on`), EL2 (`virtualization=on`), or EL1.
2. **Zircon-Style AArch64 Spinlock (`spinlock.h`)**:
   * Implements `arch_spin_lock` and `arch_spin_unlock` modeled on `zircon_org/kernel/arch/arm64/spinlock.cpp`, storing `arch_curr_cpu_num() + 1` in the lock word and using `sevl` / `wfe` / `ldaxr` / `stxr` / `stlr`.
3. **Serialized Kernel Debuglog (`debuglog.c`)**:
   * Implements `dprintf(level, fmt, ...)` over the PL011 UART (`0x09000000`), serialized by `dlog_lock` so all 4 CPU cores log cleanly without character interleaving.
4. **SMP Bringup in `lk_main()` (`main.c`)**:
   * CPU 0 initializes the early debuglog, prints the kernel memory map and DTB address, wakes CPUs 1–3 via `psci_cpu_on`, waits for all 4 CPUs to come online in EL1, and transitions into `thread_become_idle()`.

## Building & Running
```bash
make
make run      # Boots from EL3 -> EL2 -> EL1
make run_el2  # Boots from EL2 -> EL1
make run_el1  # Boots directly in EL1
```
