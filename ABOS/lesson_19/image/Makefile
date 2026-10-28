# =========================================================================
# image/Makefile - Final Image Packaging (kernel.elf, kernel.img, disk.img)
# =========================================================================
ROOT_DIR ?= $(abspath ..)
include $(ROOT_DIR)/config.mk

KERNEL_C_SRCS := $(wildcard $(ROOT_DIR)/kernel/core/*.c) \
                 $(wildcard $(ROOT_DIR)/kernel/mm/*.c) \
                 $(wildcard $(ROOT_DIR)/kernel/dev/*.c)
KERNEL_S_SRCS := $(wildcard $(ROOT_DIR)/kernel/core/*.S)

USER_SYS_SRCS := $(ROOT_DIR)/user/ulibc/ulibc.c \
                 $(wildcard $(ROOT_DIR)/user/drivers/*.c) \
                 $(wildcard $(ROOT_DIR)/user/userboot/*.c) \
                 $(wildcard $(ROOT_DIR)/user/shell/*.c)

SYS_OBJS      := $(KERNEL_C_SRCS:.c=.o) \
                 $(USER_SYS_SRCS:.c=.o) \
                 $(KERNEL_S_SRCS:.S=.o)

APP_SRCS      := $(wildcard $(ROOT_DIR)/user/apps/*.c)
APP_NAMES     := $(basename $(notdir $(APP_SRCS)))
APP_BINS      := $(addsuffix .bin,$(APP_NAMES))

KERNEL_ELF    := kernel.elf
KERNEL_IMG    := kernel.img
DISK_IMG      := disk.img

.PHONY: all clean

all: $(KERNEL_IMG) $(DISK_IMG)
	@:

$(KERNEL_ELF): $(SYS_OBJS) $(KERNEL_LD)
	@echo "  [LD]      image/$@ (using kernel linker.ld)"
	@$(LD) $(KERNEL_LDFLAGS) -o $@ $(SYS_OBJS)

$(KERNEL_IMG): $(KERNEL_ELF)
	@echo "  [OBJCOPY] image/$@"
	@$(OBJCOPY) -O binary $< $@

$(DISK_IMG): $(APP_BINS) make_tar_disk.py
	@echo "  [TAR]     image/$@ (motd.txt, zircon.txt, $(APP_BINS))"
	@python3 make_tar_disk.py

clean:
	@rm -f $(KERNEL_ELF) $(KERNEL_IMG) $(DISK_IMG) *.elf *.bin
