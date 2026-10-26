# =========================================================================
# ABOS Lesson 16: Strict EL1 (kernel/) vs. EL0 (user/) Microkernel Layout
#
# Directory Layout:
#   include/         - Shared kernel, MM, object, syscall, and IPC headers
#   kernel/          - Everything running in EL1 (Privileged Microkernel):
#     kernel/core/   - Boot ASM, exceptions, SMP scheduler, objects, syscalls, main.c
#     kernel/mm/     - Physical Memory Manager (pmm.c), kernel heap, 4-level MMU
#     kernel/dev/    - EL1 platform interrupt controller (gic.c), timer, early debuglog
#   user/            - Everything running in EL0 (Unprivileged User-Space):
#     user/ulibc/    - C Runtime (crt0.S, crt_env.c) & Syscall Library (ulibc.c -> libulibc.a)
#     user/drivers/  - EL0 User-Space Drivers: console_srv.c (UART) & fs_srv.c (VirtIO-Blk TAR)
#     user/userboot/ - EL0 root bootstrap process
#     user/shell/    - EL0 interactive capability shell (abos-shell)
#     user/apps/     - Standalone EL0 applications (calc.c -> image/calc.bin)
#   image/           - Image packaging script & outputs (kernel.img, calc.bin, disk.img)
# =========================================================================

PREBUILT_GCC  := ../../../zircon_org/prebuilt/downloads/gcc/bin/aarch64-elf-gcc
PREBUILT_QEMU := ../../../zircon_org/prebuilt/downloads/qemu/bin/qemu-system-aarch64

ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-linux-gnu-
else ifneq ($(shell which aarch64-elf-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-elf-
else ifneq ($(wildcard $(PREBUILT_GCC)),)
    CROSS_COMPILE ?= ../../../zircon_org/prebuilt/downloads/gcc/bin/aarch64-elf-
else
    CROSS_COMPILE ?= aarch64-linux-gnu-
endif

ifneq ($(shell which qemu-system-aarch64 2>/dev/null),)
    QEMU ?= qemu-system-aarch64
else ifneq ($(wildcard $(PREBUILT_QEMU)),)
    QEMU ?= $(PREBUILT_QEMU)
else
    QEMU ?= qemu-system-aarch64
endif

CC            := $(CROSS_COMPILE)gcc
LD            := $(CROSS_COMPILE)ld
AR            := $(CROSS_COMPILE)ar
OBJCOPY       := $(CROSS_COMPILE)objcopy

# Subdirectories
INC_DIR       := include
K_CORE_DIR    := kernel/core
K_MM_DIR      := kernel/mm
K_DEV_DIR     := kernel/dev
ULIBC_DIR     := user/ulibc
U_DRIVERS_DIR := user/drivers
USERBOOT_DIR  := user/userboot
SHELL_DIR     := user/shell
APPS_DIR      := user/apps
IMAGE_DIR     := image

CFLAGS  := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
           -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g \
           -I$(INC_DIR) -I$(ULIBC_DIR)
LDFLAGS := -n -T linker.ld

# EL1 Kernel sources
KERNEL_C_SRCS := $(wildcard $(K_CORE_DIR)/*.c) \
                 $(wildcard $(K_MM_DIR)/*.c) \
                 $(wildcard $(K_DEV_DIR)/*.c)
KERNEL_S_SRCS := $(wildcard $(K_CORE_DIR)/*.S)

# EL0 User-Space system sources
USER_SYS_SRCS := $(ULIBC_DIR)/ulibc.c \
                 $(wildcard $(U_DRIVERS_DIR)/*.c) \
                 $(wildcard $(USERBOOT_DIR)/*.c) \
                 $(wildcard $(SHELL_DIR)/*.c)

HEADERS       := $(wildcard $(INC_DIR)/*.h) $(wildcard $(ULIBC_DIR)/*.h)

# User C Runtime & Static Library (libulibc.a)
CRT0_OBJ      := $(ULIBC_DIR)/crt0.o
CRT_ENV_OBJ   := $(ULIBC_DIR)/crt_env.o
ULIBC_OBJ     := $(ULIBC_DIR)/ulibc.o
ULIBC_A       := $(ULIBC_DIR)/libulibc.a

OBJS          := $(KERNEL_C_SRCS:.c=.o) \
                 $(USER_SYS_SRCS:.c=.o) \
                 $(KERNEL_S_SRCS:.S=.o)

# Output artifacts in image/
KERNEL_ELF    := $(IMAGE_DIR)/kernel.elf
KERNEL_IMG    := $(IMAGE_DIR)/kernel.img
CALC_OBJ      := $(APPS_DIR)/calc.o
CALC_ELF      := $(IMAGE_DIR)/calc.elf
CALC_BIN      := $(IMAGE_DIR)/calc.bin
DISK_IMG      := $(IMAGE_DIR)/disk.img

.PHONY: all run clean check-tools install-tools

all: check-tools $(ULIBC_A) $(KERNEL_IMG) $(DISK_IMG)

check-tools:
	@if ! command -v $(CC) >/dev/null 2>&1 && [ ! -x "$(CC)" ]; then \
		echo "Error: Cross-compiler '$(CC)' not found!"; \
		echo "Run './install_tools.sh' (or 'make install-tools') to install AArch64 GCC & QEMU."; \
		exit 1; \
	fi

install-tools:
	./install_tools.sh

%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(HEADERS)
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Archive user/ulibc static C library (libulibc.a)
$(ULIBC_A): $(CRT0_OBJ) $(CRT_ENV_OBJ) $(ULIBC_OBJ)
	@echo "Archiving $@..."
	$(AR) rcs $@ $^

$(KERNEL_ELF): $(OBJS) linker.ld
	@echo "Linking $@..."
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL_IMG): $(KERNEL_ELF)
	@echo "Creating $@..."
	$(OBJCOPY) -O binary $< $@

$(CALC_ELF): $(CRT0_OBJ) $(CALC_OBJ) $(ULIBC_A) $(APPS_DIR)/user_calc.ld
	@echo "Linking standalone user app $@ (crt0.o + calc.o + libulibc.a) at VA 0x20000000..."
	$(LD) -n -T $(APPS_DIR)/user_calc.ld -o $@ $(CRT0_OBJ) $(CALC_OBJ) $(ULIBC_A)

$(CALC_BIN): $(CALC_ELF)
	@echo "Creating standalone binary $@..."
	$(OBJCOPY) -O binary $< $@

$(DISK_IMG): $(CALC_BIN) $(IMAGE_DIR)/make_tar_disk.py
	@echo "Packing $@ (motd.txt, zircon.txt, calc.bin)..."
	python3 $(IMAGE_DIR)/make_tar_disk.py

run: all
	@if ! command -v $(QEMU) >/dev/null 2>&1 && [ ! -x "$(QEMU)" ]; then \
		echo "Error: '$(QEMU)' not found! Run './install_tools.sh' first."; \
		exit 1; \
	fi
	@echo "Running ABOS Lesson 16 in QEMU..."
	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel $(KERNEL_IMG) -nographic -serial mon:stdio \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0

clean:
	@echo "Cleaning up build artifacts and image/ outputs..."
	rm -f $(OBJS) $(CRT0_OBJ) $(CRT_ENV_OBJ) $(ULIBC_A) $(CALC_OBJ) $(KERNEL_ELF) $(KERNEL_IMG) $(CALC_ELF) $(CALC_BIN) $(DISK_IMG)
