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
OBJS	+= bios/intfake.o
OBJS	+= bios/int10.o

CFLAGS	+= $(CPPFLAGS) -Iinclude -g

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
bios/intfake.o: bios/intfake.S bios/intfake-real.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) -c bios/intfake-real.S -o bios/intfake-real.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/intfake-real.o bios/intfake-real.bin
	$(Q) $(CC) $(CFLAGS) -c bios/intfake.S -o bios/intfake.o

bios/int10.o: bios/int10.S bios/int10-real.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) -c bios/int10-real.S -o bios/int10-real.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/int10-real.o bios/int10-real.bin
	$(Q) $(CC) $(CFLAGS) -c bios/int10.S -o bios/int10.o


clean:
	$(E) "  CLEAN"
	$(Q) rm -f bios/*.bin
	$(Q) rm -f bios/*.o
	$(Q) rm -f $(OBJS) $(PROGRAM)
.PHONY: clean
