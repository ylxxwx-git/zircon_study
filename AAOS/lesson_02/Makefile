# =================================================================
# Makefile for AArch64 bare-metal kernel targeting QEMU 'virt'
# =================================================================

# Define the cross-compiler toolchain for 64-bit ARM
# Ensure you have 'aarch64-linux-gnu-gcc' installed
CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld

# Source files
C_SOURCES = kernel.c output.c
ASM_SOURCES = boot.S

# Object files
OBJS = $(C_SOURCES:.c=.o) $(ASM_SOURCES:.S=.o)

# Target ELF executable
TARGET = kernel.elf

# Compiler flags for C and Assembly
# -O0: No optimization (good for bare-metal debugging)
# -Wall -Wextra: Enable warnings
# -nostdlib -nostartfiles: Crucial for bare-metal (no standard libraries or startup files)
# -mcpu=cortex-a53: Targets a common 64-bit ARM core
# -ffreestanding: Compliance for a free-standing environment
# -g: Include debug symbols
CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g

# Linker flags
# -n: Tells the linker to make text and data sections read-only
# -T linker.ld: Use our custom linker script
LDFLAGS = -n -T linker.ld

.PHONY: all clean run

all: $(TARGET)

# Rule to link the object files into the final ELF executable
$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Rule to compile C files
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to assemble Assembly files
%.o: %.s
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to run the kernel in QEMU
# -M virt: Emulate the standard AArch64 virtual machine
# -cpu cortex-a53: Select the specific CPU model
# -kernel $(TARGET): Load our built kernel image
# -nographic -serial mon:stdio: Redirect the virtual serial port output to the host console
run: $(TARGET)
	@echo "Running kernel in QEMU (EL3 Default)..."
	qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 -kernel $(TARGET) -nographic -serial mon:stdio

run_el2: $(TARGET)
	@echo "Running kernel in QEMU (EL2 Forced)..."
	qemu-system-aarch64 -M virt,virtualization=on -cpu cortex-a53 -smp 4 -kernel $(TARGET) -nographic -serial mon:stdio

clean:
	@echo "Cleaning up..."
	rm -f $(OBJS) $(TARGET)