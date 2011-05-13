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

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C pit clean
	$(MAKE) -C boot clean
.PHONY: clean
