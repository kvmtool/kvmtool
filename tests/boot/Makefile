NAME	:= init

OBJ	:= $(NAME).o
MKISOFS := $(shell command -v mkisofs 2>/dev/null)
ifeq ($(MKISOFS),)
MKISOFS := $(shell command -v xorrisofs 2>/dev/null)
endif

all: $(OBJ)
	rm -rf rootfs
	mkdir rootfs
	gcc -static init.c -o rootfs/init
ifdef MKISOFS
	$(MKISOFS) -o boot_test.iso rootfs
else
	$(error "mkisofs or xorriso needed to build boot_test.iso")
endif

clean:
	rm -rf rootfs boot_test.iso
.PHONY: clean
