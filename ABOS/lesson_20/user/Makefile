# =========================================================================
# user/Makefile - EL0 User-Space Subsystems (ulibc, drivers, userboot, shell, apps)
# =========================================================================
ROOT_DIR ?= $(abspath ..)
include $(ROOT_DIR)/config.mk

SUBDIRS := ulibc drivers userboot shell apps

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

# Ensure ulibc (crt0.o + libulibc.a) is built before standalone servers/shells/apps
drivers shell apps: ulibc

$(SUBDIRS):
	@$(MAKE) --no-print-directory -C $@ all

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) --no-print-directory -C $$dir clean; \
	done
