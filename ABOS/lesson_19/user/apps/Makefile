# =========================================================================
# user/apps/Makefile - Standalone EL0 User-Space Applications
#
# All user-space applications share the common linker script $(USER_LD)
# (user/link.ld), C runtime startup $(CRT0_OBJ), and library $(ULIBC_A).
# Adding any new <app>.c in user/apps/ automatically builds:
#   - $(IMAGE_DIR)/<app>.elf
#   - $(IMAGE_DIR)/<app>.bin
# =========================================================================
ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/config.mk

APP_SRCS := $(wildcard *.c)
APP_OBJS := $(APP_SRCS:.c=.o)
APP_NAMES := $(basename $(APP_SRCS))
APP_ELFS := $(addprefix $(IMAGE_DIR)/,$(addsuffix .elf,$(APP_NAMES)))
APP_BINS := $(addprefix $(IMAGE_DIR)/,$(addsuffix .bin,$(APP_NAMES)))

.PRECIOUS: $(APP_OBJS) $(APP_ELFS)
.PHONY: all clean

all: $(APP_BINS)
	@:

$(ULIBC_A) $(CRT0_OBJ):
	@$(MAKE) --no-print-directory -C $(ULIBC_DIR) all

$(IMAGE_DIR)/%.elf: %.o $(CRT0_OBJ) $(ULIBC_A) $(USER_LD)
	@echo "  [LD]      image/$(notdir $@) (using common user/link.ld)"
	@$(LD) $(USER_LDFLAGS) -o $@ $(CRT0_OBJ) $< $(ULIBC_A)

$(IMAGE_DIR)/%.bin: $(IMAGE_DIR)/%.elf
	@echo "  [OBJCOPY] image/$(notdir $@)"
	@$(OBJCOPY) -O binary $< $@

clean:
	@rm -f $(APP_OBJS) $(APP_ELFS) $(APP_BINS)
