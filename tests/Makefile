all: kernel pit

kernel:
	$(MAKE) -C kernel
.PHONY: kernel

pit:
	$(MAKE) -C pit
.PHONY: pit

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C pit clean
.PHONY: clean
