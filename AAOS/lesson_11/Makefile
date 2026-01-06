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

# User Space Sources
USER_C_SOURCES = $(USER_DIR)/lib.c $(USER_DIR)/shell.c
USER_ASM_SOURCES = $(USER_DIR)/start.S
USER_OBJS = $(USER_ASM_SOURCES:.S=.o) $(USER_C_SOURCES:.c=.o)

# User Space Flags (No stdlib, freestanding)
USER_CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g -fno-pie -fno-pic -fno-builtin -mgeneral-regs-only -I$(INCLUDE_DIR)

# User Space Linker Flags
USER_LDFLAGS = -n -Ttext=0x0

.PHONY: all clean run

all: kernel.img disk.img

# Build User Shell Binary
shell.bin: $(USER_OBJS)
	@echo "Linking shell.bin..."
	$(LD) $(USER_LDFLAGS) -o shell.elf $(USER_OBJS)
	$(OBJCOPY) -O binary shell.elf shell.bin

# Create Disk Image
disk.img: shell.bin
	@echo "Creating disk.img..."
	mkdir -p rootfs
	cp shell.bin rootfs/shell.bin
	tar -cf disk.img -C rootfs .

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

# User Space Rules
$(USER_DIR)/%.o: $(USER_DIR)/%.c
	@echo "Compiling User $<..."
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_DIR)/%.o: $(USER_DIR)/%.S
	@echo "Assembling User $<..."
	$(CC) $(USER_CFLAGS) -c $< -o $@

run: kernel.img disk.img
	@echo "Running kernel in QEMU..."
	qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 -kernel kernel.img -nographic -serial mon:stdio -drive file=disk.img,format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0

clean:
	@echo "Cleaning up..."
	rm -f $(OBJS) $(USER_OBJS) kernel.elf kernel.img shell.elf shell.bin disk.img
	rm -rf rootfs
