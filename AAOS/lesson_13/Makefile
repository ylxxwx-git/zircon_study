# =================================================================
# Makefile for AArch64 bare-metal kernel targeting QEMU 'virt'
# =================================================================

CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

# Directories
KERNEL_DIR = kernel
DRIVERS_DIR = drivers
MM_DIR = mm
FS_DIR = fs
USER_DIR = user
INCLUDE_DIR = include
LIB_DIR = lib

# Source files
KERNEL_SRCS = $(wildcard $(KERNEL_DIR)/*.c)
DRIVERS_SRCS = $(wildcard $(DRIVERS_DIR)/*.c)
MM_SRCS = $(wildcard $(MM_DIR)/*.c)
FS_SRCS = $(wildcard $(FS_DIR)/*.c)
LIB_SRCS = $(wildcard $(LIB_DIR)/*.c)
ASM_SRCS = $(wildcard $(KERNEL_DIR)/*.S)

# Object files
OBJS = $(KERNEL_SRCS:.c=.o) $(DRIVERS_SRCS:.c=.o) $(MM_SRCS:.c=.o) $(FS_SRCS:.c=.o) $(LIB_SRCS:.c=.o) $(ASM_SRCS:.S=.o)

# Compiler flags
# Add -I flags for all source directories to allow easy includes
CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g -fno-pie -fno-pic -fno-builtin -mgeneral-regs-only \
         -I$(INCLUDE_DIR) -I$(KERNEL_DIR) -I$(DRIVERS_DIR) -I$(MM_DIR) -I$(FS_DIR) -I$(LIB_DIR)

# Linker flags
LDFLAGS = -n -T linker.ld

# User Space Build delegated to user/Makefile

.PHONY: all clean run user_build

all: kernel.img disk.img

# Build User Shell Binary (Delegate)
user_build:
	$(MAKE) -C $(USER_DIR)

# Create Disk Image
disk.img: user_build
	cp $(USER_DIR)/disk.img .

# Link Kernel
kernel.elf: $(OBJS) linker.ld
	@echo "Linking kernel.elf..."
	$(LD) $(LDFLAGS) -o kernel.elf $(OBJS)

kernel.img: kernel.elf
	@echo "Creating kernel.img..."
	$(OBJCOPY) -O binary kernel.elf kernel.img

# Generic rules for C files
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Generic rules for ASM files
%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.img disk.img
	@echo "Running kernel in QEMU..."
	qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 -kernel kernel.img -nographic -serial mon:stdio -drive file=disk.img,format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0

clean:
	@echo "Cleaning up..."
	rm -f $(OBJS) kernel.elf kernel.img disk.img
	rm -f $(KERNEL_DIR)/*.o $(DRIVERS_DIR)/*.o $(MM_DIR)/*.o $(FS_DIR)/*.o $(LIB_DIR)/*.o
	rm -rf rootfs
	$(MAKE) -C $(USER_DIR) clean
