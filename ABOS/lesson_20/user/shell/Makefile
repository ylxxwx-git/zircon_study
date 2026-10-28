# =========================================================================
# user/shell/Makefile - Standalone EL0 Interactive Capability Shell
#
# Links shell.bin as a standalone EL0 executable at VA 0x20000000 using
# the common linker script $(USER_LD) (user/link.ld).
# =========================================================================
ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/config.mk

OBJS      := shell.o
SHELL_ELF := $(IMAGE_DIR)/shell.elf
SHELL_BIN := $(IMAGE_DIR)/shell.bin

.PRECIOUS: $(OBJS) $(SHELL_ELF)
.PHONY: all clean

all: $(SHELL_BIN)
	@:

$(ULIBC_A) $(CRT0_OBJ):
	@$(MAKE) --no-print-directory -C $(ULIBC_DIR) all

$(SHELL_ELF): shell.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)

$(SHELL_BIN): $(SHELL_ELF)
	@echo "  [OBJCOPY] image/$(notdir $@)"
	@$(OBJCOPY) -O binary $< $@

clean:
	@rm -f $(OBJS) $(SHELL_ELF) $(SHELL_BIN)
