# =========================================================================
# image/Makefile - Final Image Packaging (kernel.img, bootfs.tar, disk.img)
#
# Notice that console-srv, fs-srv, and shell are completely separate from
# kernel.elf:
#   1. kernel.elf links ONLY EL1 kernel + userboot (passed to QEMU -kernel)
#   2. bootfs.tar packs console-srv.bin, fs-srv.bin, and shell.bin (passed to QEMU -initrd)
#   3. disk.img packs motd.txt, zircon.txt, and standalone apps (passed to QEMU -drive)
# =========================================================================
ROOT_DIR ?= $(abspath ..)
include $(ROOT_DIR)/config.mk

KERNEL_C_SRCS := $(wildcard $(ROOT_DIR)/kernel/core/*.c) \
                 $(wildcard $(ROOT_DIR)/kernel/mm/*.c) \
                 $(wildcard $(ROOT_DIR)/kernel/dev/*.c)
KERNEL_S_SRCS := $(wildcard $(ROOT_DIR)/kernel/core/*.S)

# Only userboot and ulibc syscall stubs are linked into the kernel image
USERBOOT_OBJS := $(ROOT_DIR)/user/ulibc/ulibc.o \
                 $(ROOT_DIR)/user/userboot/userboot.o

SYS_OBJS      := $(KERNEL_C_SRCS:.c=.o) \
                 $(USERBOOT_OBJS) \
                 $(KERNEL_S_SRCS:.S=.o)

BOOTFS_BINS   := console-srv.bin fs-srv.bin shell.bin
BOOTFS_TAR    := bootfs.tar

APP_SRCS      := $(wildcard $(ROOT_DIR)/user/apps/*.c)
APP_NAMES     := $(basename $(notdir $(APP_SRCS)))
APP_BINS      := $(addsuffix .bin,$(APP_NAMES))

KERNEL_ELF    := kernel.elf
KERNEL_IMG    := kernel.img
DISK_IMG      := disk.img

.PHONY: all clean

all: $(KERNEL_IMG) $(BOOTFS_TAR) $(DISK_IMG)
	@:

$(KERNEL_ELF): $(SYS_OBJS) $(KERNEL_LD)
	@echo "  [LD]      image/$@ (EL1 kernel + userboot only)"
	@$(LD) $(KERNEL_LDFLAGS) -o $@ $(SYS_OBJS)

$(KERNEL_IMG): $(KERNEL_ELF)
	@echo "  [OBJCOPY] image/$@"
	@$(OBJCOPY) -O binary $< $@

$(BOOTFS_TAR): $(BOOTFS_BINS) make_bootfs.py
	@echo "  [BOOTFS]  image/$@ ($(BOOTFS_BINS))"
	@python3 make_bootfs.py

$(DISK_IMG): $(APP_BINS) make_tar_disk.py
	@echo "  [TAR]     image/$@ (motd.txt, zircon.txt, $(APP_BINS))"
	@python3 make_tar_disk.py

clean:
	@rm -f $(BOOTFS_TAR) $(KERNEL_ELF) $(KERNEL_IMG) $(DISK_IMG) *.elf *.bin *.o
