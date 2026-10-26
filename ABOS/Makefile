SUBDIRS := $(wildcard lesson_*)

.PHONY: all clean $(SUBDIRS)

all:
	@for dir in $(SUBDIRS); do \
		if [ -f $$dir/Makefile ]; then \
			echo "Building $$dir..."; \
			$(MAKE) -C $$dir; \
		fi \
	done

clean:
	@for dir in $(SUBDIRS); do \
		if [ -f $$dir/Makefile ]; then \
			echo "Cleaning $$dir..."; \
			$(MAKE) -C $$dir clean; \
		fi \
	done

install-tools:
	./install_tools.sh
