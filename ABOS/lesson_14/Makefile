# =========================================================================
# Makefile - ABOS Lesson 14: Complete Microkernel OS (EL0 Drivers, FS & Shell)
# =========================================================================
PREBUILT_DIR = /usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads
TOOLCHAIN    = $(PREBUILT_DIR)/gcc/bin/aarch64-elf-
QEMU         = $(PREBUILT_DIR)/qemu/bin/qemu-system-aarch64

CC      = $(TOOLCHAIN)gcc
LD      = $(TOOLCHAIN)ld
OBJCOPY = $(TOOLCHAIN)objcopy

CFLAGS  = -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
          -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g
LDFLAGS = -n -T linker.ld

C_SRCS   = main.c debuglog.c exceptions_c.c gic.c timer.c dtb.c pmm.c heap.c mmu.c thread.c wait.c object.c syscalls.c
ASM_SRCS = start.S exceptions.S switch.S uspace_entry.S
OBJS     = $(C_SRCS:.c=.o) $(ASM_SRCS:.S=.o)

.PHONY: all run clean

all: kernel.img disk.img

disk.img: make_tar_disk.py
	@echo "Creating USTAR TAR disk.img..."
	@python3 make_tar_disk.py

kernel.elf: $(OBJS) linker.ld
	@echo "Linking $@..."
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

kernel.img: kernel.elf
	@echo "Creating $@..."
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.img disk.img
	@echo "Running ABOS Lesson 14 in QEMU..."
	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel kernel.img -nographic -serial mon:stdio \
	        -drive file=disk.img,format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0

clean:
	rm -f $(OBJS) kernel.elf kernel.img disk.img
