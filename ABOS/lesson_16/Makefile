# =========================================================================
# ABOS Lesson 16: Modular Microkernel OS Build System (with ulibc & crt0)
#
# Directory Layout:
#   include/         - Shared kernel, MM, object, syscall, and IPC headers
#   kernel/          - Core microkernel (boot ASM, exceptions, scheduler, objects, syscalls)
#   mm/              - Memory management (PMM, kernel heap, 4-level ARMv8 MMU)
#   drivers/         - Kernel platform drivers (GIC, Timer) + EL0 servers (console-srv, fs-srv)
#   user/ulibc/      - C Runtime (crt0.S, crt_env.c) & Wrapped Syscall Library (ulibc.c -> libulibc.a)
#   user/userboot/   - EL0 root bootstrap process
#   user/shell/      - EL0 interactive capability shell (abos-shell)
#   user/apps/       - Standalone EL0 applications (calc.c -> image/calc.bin)
#   image/           - Output images (kernel.elf, kernel.img, calc.bin, disk.img)
# =========================================================================
TOOLCHAIN_DIR := /usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads
GCC_BIN       := $(TOOLCHAIN_DIR)/gcc/bin
QEMU_BIN      := $(TOOLCHAIN_DIR)/qemu/bin

CROSS_COMPILE := $(GCC_BIN)/aarch64-elf-
CC            := $(CROSS_COMPILE)gcc
LD            := $(CROSS_COMPILE)ld
AR            := $(CROSS_COMPILE)ar
OBJCOPY       := $(CROSS_COMPILE)objcopy
QEMU          := $(QEMU_BIN)/qemu-system-aarch64

# Subdirectories
INC_DIR       := include
KERNEL_DIR    := kernel
MM_DIR        := mm
DRIVERS_DIR   := drivers
ULIBC_DIR     := user/ulibc
USERBOOT_DIR  := user/userboot
SHELL_DIR     := user/shell
APPS_DIR      := user/apps
IMAGE_DIR     := image

CFLAGS  := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
           -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g \
           -I$(INC_DIR) -I$(ULIBC_DIR)
LDFLAGS := -n -T linker.ld

# Source files grouped by subsystem
KERNEL_C_SRCS := $(wildcard $(KERNEL_DIR)/*.c)
KERNEL_S_SRCS := $(wildcard $(KERNEL_DIR)/*.S)
MM_SRCS       := $(wildcard $(MM_DIR)/*.c)
DRIVERS_SRCS  := $(wildcard $(DRIVERS_DIR)/*.c)
USER_SYS_SRCS := $(ULIBC_DIR)/ulibc.c \
                 $(wildcard $(USERBOOT_DIR)/*.c) \
                 $(wildcard $(SHELL_DIR)/*.c)

HEADERS       := $(wildcard $(INC_DIR)/*.h) $(wildcard $(ULIBC_DIR)/*.h)

# User C Runtime & Static Library (libulibc.a)
CRT0_OBJ      := $(ULIBC_DIR)/crt0.o
CRT_ENV_OBJ   := $(ULIBC_DIR)/crt_env.o
ULIBC_OBJ     := $(ULIBC_DIR)/ulibc.o
ULIBC_A       := $(ULIBC_DIR)/libulibc.a

OBJS          := $(KERNEL_C_SRCS:.c=.o) \
                 $(MM_SRCS:.c=.o) \
                 $(DRIVERS_SRCS:.c=.o) \
                 $(USER_SYS_SRCS:.c=.o) \
                 $(KERNEL_S_SRCS:.S=.o)

# Output artifacts in image/
KERNEL_ELF    := $(IMAGE_DIR)/kernel.elf
KERNEL_IMG    := $(IMAGE_DIR)/kernel.img
CALC_OBJ      := $(APPS_DIR)/calc.o
CALC_ELF      := $(IMAGE_DIR)/calc.elf
CALC_BIN      := $(IMAGE_DIR)/calc.bin
DISK_IMG      := $(IMAGE_DIR)/disk.img

.PHONY: all run clean

all: $(ULIBC_A) $(KERNEL_IMG) $(DISK_IMG)

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

run: $(KERNEL_IMG) $(DISK_IMG)
	@echo "Running ABOS Lesson 16 in QEMU..."
	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel $(KERNEL_IMG) -nographic -serial mon:stdio \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0

clean:
	@echo "Cleaning up build artifacts and image/ outputs..."
	rm -f $(OBJS) $(CRT_ENV_OBJ) $(ULIBC_A) $(CALC_OBJ) $(KERNEL_ELF) $(KERNEL_IMG) $(CALC_ELF) $(CALC_BIN) $(DISK_IMG)
