# Difficult Issues & Learnings

## 1. QEMU Load Address vs. RAM Base
**Issue:**
We initially assumed the kernel starts at `0x40000000` (Start of RAM). However, QEMU loads the kernel binary at `0x40080000` (512KB offset).

**Reason:**
QEMU reserves the first 512KB of RAM (`0x40000000` - `0x40080000`) for:
-   **Device Tree Blob (DTB)**: Placed at `0x40000000`.
-   **Spin Tables / Mailboxes**: Used for secondary core booting (if not using PSCI).

**Fix:**
-   Updated `linker.ld` to set the load address (`.`) to `0x40080000`.
-   Ensured the stack and other memory regions respect this offset or use available memory above the kernel.

## 2. Compiler Flag: `-mgeneral-regs-only`
**Issue:**
Unexpected crashes or undefined behavior when using floating-point registers in the kernel, especially during context switches or interrupt handling.

**Reason:**
By default, the compiler may use SIMD/FP registers (like `q0`-`q31`) for optimization (e.g., `memcpy` or large struct copies).
-   The kernel does not save/restore these registers during every interrupt/context switch (to save time).
-   If user space is using FP registers and an interrupt occurs, the kernel might clobber them if it uses FP instructions.

**Fix:**
-   Added `-mgeneral-regs-only` to `CFLAGS`.
-   This forces the compiler to use **only** general-purpose registers (`x0`-`x30`), preventing accidental clobbering of user FP state.
-   **Note:** If we explicitly need FP in the kernel (e.g., for crypto), we must manually enable it and save/restore the context.

## 3. Stack Pointer Initialization (PA vs VA)
**Issue:**
Silent crashes when enabling MMU because the stack pointer was set to a Physical Address (`0x48000000`) which wasn't mapped in the High Kernel Page Table (`TTBR1`).

**Reason:**
-   `boot_pa.S` runs in Physical Address space (Identity Mapped).
-   `boot_va.S` runs in Virtual Address space (High Kernel).
-   `TTBR1` only covers `0xFFFFFFFF_80000000` and above.
-   Accessing `0x48000000` via `TTBR1` causes a Translation Fault.

**Fix:**
-   Updated `linker.ld` to define `_stack_top` as a High Kernel Virtual Address:
    ```ld
    _stack_top = 0xFFFFFFFF00000000 + BASE_ADDR + 0x08000000;
    ```
-   This ensures the stack pointer is valid when running in the High Kernel.

## 4. User Space Process Isolation
**Issue:**
Modifying the global `idmap_table` for user mappings breaks isolation and is unsafe.

**Fix:**
-   Implemented per-process L1 Page Tables.
-   Each process gets its own L1 table, and `TTBR0_EL1` is switched during context switch (or process creation).
