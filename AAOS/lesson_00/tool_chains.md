# Arm Toolchains for Cross-Compilation

When building Arm binaries on other platforms (like x86_64 Linux or macOS), you need a **cross-compilation toolchain**. The naming convention for these toolchains usually follows the pattern: `arch-vendor-os-abi`.

Understanding the difference is critical because they link against different Standard C Libraries (libc) which have different assumptions about the runtime environment.

## Common Toolchains

### 1. `aarch64-linux-gnu-`
*   **Architecture**: AArch64 (64-bit Arm)
*   **Target OS**: Linux
*   **C Library**: **glibc** (GNU C Library)
*   **Description**: This toolchain is designed for building **user-space applications** that run on top of a Linux Operating System. It links against `glibc`, which makes heavy use of Linux system calls (syscalls) for operations like file I/O, memory allocation, and threading.

### 2. `aarch64-none-elf-`
*   **Architecture**: AArch64 (64-bit Arm)
*   **Target OS**: None (Bare Metal)
*   **C Library**: **newlib** (Red Hat's embedded C library)
*   **Description**: This toolchain is designed for **bare metal programming**—running code directly on hardware without an OS. It uses `newlib`, which is optimized for embedded systems and does not assume a Linux kernel is present. It provides stub functions that you must implement if you want standard library features.
    *   `none`: Indicates no specific vendor.
    *   `elf`: Indicates the output binary format is ELF (Executable and Linkable Format).

### 3. `arm-none-eabi-`
*   **Architecture**: Arm (32-bit)
*   **Target OS**: None (Bare Metal)
*   **C Library**: **newlib**
*   **Description**: The 32-bit equivalent of `aarch64-none-elf`. Used for Cortex-M microcontrollers (STM32, Arduino Portenta) and 32-bit bare metal on Cortex-A (like Raspberry Pi in 32-bit mode).
    *   `eabi`: Embedded Application Binary Interface.

## Comparison Table

| Toolchain | Bit Width | Target OS | Std Lib | Typical Use Case |
| :--- | :--- | :--- | :--- | :--- |
| **`aarch64-linux-gnu-`** | 64-bit | Linux | `glibc` | **Apps**: Web servers, CLI tools, Python, etc. running on Ubuntu/Debian Arm64. |
| **`aarch64-none-elf-`** | 64-bit | Bare Metal | `newlib` | **System**: OS Kernels, Bootloaders, Firmware, Hypervisors. |
| **`arm-linux-gnueabihf-`** | 32-bit | Linux | `glibc` | **Apps**: Apps running on 32-bit Linux (e.g., Raspbian). |
| **`arm-none-eabi-`** | 32-bit | Bare Metal | `newlib` | **System**: Microcontrollers, 32-bit OS/Firmware. |

## Which one to choose?

### Scenario A: "I am writing an application to run on my Raspberry Pi running Linux."
**Choose**: `aarch64-linux-gnu-`
*   **Reason**: Your application needs the full power of the Linux OS. You want `printf` to print to the terminal immediately, you want to open files using `fopen`, and you want to use standard libraries that depend on the kernel.

### Scenario B: "I am writing my own Operating System (like in this course)."
**Choose**: `aarch64-none-elf-`
*   **Reason**: You **are** the kernel. You cannot rely on Linux system calls because Linux isn't running yet—you are writing the code that *would* handle those calls. `aarch64-none-elf` allows you to compile code that runs with no underlying support, giving you full control over the hardware.

### Scenario C: "I am building the Linux Kernel itself."
**Choose**: `aarch64-linux-gnu-` (usually)
*   **Reason**: Even though the kernel is bare metal, the Linux kernel build system is designed to work with the `linux-gnu` toolchain by passing special flags (like `-nostdlib` and `-ffreestanding`) to disable the standard library linkage. However, `aarch64-none-elf` works perfectly fine (and is arguably more correct) for this too.
