# =========================================================================
# user/drivers/Makefile - Standalone EL0 User-Space Drivers
#
# Links console-srv.bin and fs-srv.bin as standalone EL0 executables at
# VA 0x20000000 using the common linker script $(USER_LD) (user/link.ld).
# =========================================================================
ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/config.mk

OBJS     := console_srv.o fs_srv.o
SRV_ELFS := $(IMAGE_DIR)/console-srv.elf $(IMAGE_DIR)/fs-srv.elf
SRV_BINS := $(IMAGE_DIR)/console-srv.bin $(IMAGE_DIR)/fs-srv.bin

.PRECIOUS: $(OBJS) $(SRV_ELFS)
.PHONY: all clean

all: $(SRV_BINS)
	@:

$(ULIBC_A) $(CRT0_OBJ):
	@$(MAKE) --no-print-directory -C $(ULIBC_DIR) all

$(IMAGE_DIR)/console-srv.elf: console_srv.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)

$(IMAGE_DIR)/fs-srv.elf: fs_srv.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)

$(IMAGE_DIR)/%.bin: $(IMAGE_DIR)/%.elf
	@echo "  [OBJCOPY] image/$(notdir $@)"
	@$(OBJCOPY) -O binary $< $@

clean:
	@rm -f $(OBJS) $(SRV_ELFS) $(SRV_BINS)
