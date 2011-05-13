NAME	:= init

OBJ	:= $(NAME).o

all: $(.o)
	rm -rf rootfs
	mkdir rootfs
	gcc -static init.c -o rootfs/init
	mkisofs rootfs > boot_test.iso

clean:
	rm -rf rootfs boot_test.iso
.PHONY: clean
