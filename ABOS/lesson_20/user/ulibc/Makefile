# =========================================================================
# user/ulibc/Makefile - EL0 C Runtime (crt0.o, crt_env.o) & Static Library (libulibc.a)
# =========================================================================
ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/config.mk

OBJS := crt0.o crt_env.o ulibc.o
LIB  := libulibc.a

.PHONY: all clean

all: $(LIB)
	@:

$(LIB): $(OBJS)
	@echo "  [AR]      user/ulibc/$@"
	@$(AR) rcs $@ $^

clean:
	@rm -f $(OBJS) $(LIB)
