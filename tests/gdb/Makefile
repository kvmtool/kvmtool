PORT ?= 12345
LKVM ?= ../../lkvm
GUEST ?= ../pit/tick.bin
PYTHON ?= python3

smoke: $(LKVM) $(GUEST)
	$(PYTHON) test-x86-gdb-stub.py --lkvm $(LKVM) --guest $(GUEST) --port $(PORT)
.PHONY: smoke
