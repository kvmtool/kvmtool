all: kernel pit boot

kernel:
	$(MAKE) -C kernel
.PHONY: kernel

pit:
	$(MAKE) -C pit
.PHONY: pit

boot:
	$(MAKE) -C boot
.PHONY: boot

gdb:
	$(MAKE) -C gdb smoke
.PHONY: gdb

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C pit clean
	$(MAKE) -C boot clean
.PHONY: clean
