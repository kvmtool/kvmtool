ifeq ($(strip $(V)),)
	E = @echo
	Q = @
else
	E = @\#
	Q =
endif
export E Q

PROGRAM	= kvm

OBJS	+= cpuid.o
OBJS	+= early_printk.o
OBJS	+= interrupt.o
OBJS	+= ioport.o
OBJS	+= kvm.o
OBJS	+= main.o
OBJS	+= mmio.o
OBJS	+= util.o
OBJS	+= bios.o
OBJS	+= bios/intfake.o
OBJS	+= bios/int10.o
OBJS	+= bios/int15.o

uname_M      := $(shell uname -m | sed -e s/i.86/i386/)
ifeq ($(uname_M),i386)
	DEFINES      += -DCONFIG_X86_32
ifeq ($(uname_M),x86_64)
	DEFINES      += -DCONFIG_X86_64
endif
endif

CFLAGS	+= $(CPPFLAGS) $(DEFINES) -Iinclude -g

WARNINGS += -Wall
WARNINGS += -Wcast-align
WARNINGS += -Wformat=2
WARNINGS += -Winit-self
WARNINGS += -Wmissing-declarations
WARNINGS += -Wmissing-prototypes
WARNINGS += -Wnested-externs
WARNINGS += -Wno-system-headers
WARNINGS += -Wold-style-definition
WARNINGS += -Wredundant-decls
WARNINGS += -Wsign-compare
WARNINGS += -Wstrict-prototypes
WARNINGS += -Wundef
WARNINGS += -Wvolatile-register-var
WARNINGS += -Wwrite-strings

CFLAGS	+= $(WARNINGS)

all: $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(E) "  LINK    " $@
	$(Q) $(CC) $(OBJS) -o $@

$(OBJS):

%.o: %.c
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

#
# BIOS assembly weirdness
#
BIOS_CFLAGS += -m32
BIOS_CFLAGS += -march=i386
BIOS_CFLAGS += -mregparm=3
bios/intfake.o: bios/intfake.S bios/intfake-real.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/intfake-real.S -o bios/intfake-real.o
	$(E) "  LD      " $@
	$(Q) ld -T bios/bios-strip.ld.S -o bios/intfake-real.bin.elf bios/intfake-real.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/intfake-real.bin.elf bios/intfake-real.bin
	$(Q) $(CC) $(CFLAGS) -c bios/intfake.S -o bios/intfake.o

bios/int10.o: bios/int10.S bios/int10-real.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/int10-real.S -o bios/int10-real.o
	$(E) "  LD      " $@
	$(Q) ld -T bios/bios-strip.ld.S -o bios/int10-real.bin.elf bios/int10-real.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/int10-real.bin.elf bios/int10-real.bin
	$(Q) $(CC) $(CFLAGS) -c bios/int10.S -o bios/int10.o

bios/int15.o: bios/int10.S bios/int15-real.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/int15-real.S -o bios/int15-real.o
	$(E) "  LD      " $@
	$(Q) ld -T bios/bios-strip.ld.S -o bios/int15-real.bin.elf bios/int15-real.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/int15-real.bin.elf bios/int15-real.bin
	$(Q) $(CC) $(CFLAGS) -c bios/int15.S -o bios/int15.o

check: $(PROGRAM)
	$(MAKE) -C tests
	./$(PROGRAM) tests/pit/tick.bin
.PHONY: check

clean:
	$(E) "  CLEAN"
	$(Q) rm -f bios/*.bin
	$(Q) rm -f bios/*.elf
	$(Q) rm -f bios/*.o
	$(Q) rm -f $(OBJS) $(PROGRAM)
.PHONY: clean

KVM_DEV	?= /dev/kvm

$(KVM_DEV):
	$(E) "  MKNOD " $@
	$(Q) mknod $@ char 10 232

devices: $(KVM_DEV)
.PHONY: devices
