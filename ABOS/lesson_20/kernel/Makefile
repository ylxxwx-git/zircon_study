# =========================================================================
# kernel/Makefile - EL1 Microkernel Subsystems (core, mm, dev)
# =========================================================================
ROOT_DIR ?= $(abspath ..)
include $(ROOT_DIR)/config.mk

SUBDIRS := core mm dev

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	@$(MAKE) --no-print-directory -C $@ all

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) --no-print-directory -C $$dir clean; \
	done
