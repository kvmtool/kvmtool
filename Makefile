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
OBJS	+= bios/c-int10.o
OBJS	+= bios/c-intfake.o

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
BIOS=-D__ASSEMBLY__ -m32 -march=i386 -Os -fno-strict-aliasing -fomit-frame-pointer

bios/c-intfake.o: bios/c-intfake.c
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) -c bios/c-intfake.c -o bios/c-intfake.o

bios/c-intfake.c: bios/intfake.bin
	$(E) "  BIN2C   " $@
	$(Q) python bios/bin2c.py -i bios/intfake.bin -o bios/c-intfake.c -n intfake

bios/intfake.o: bios/intfake.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) $(BIOS) -c -o bios/intfake.o bios/intfake.S

bios/intfake.bin: bios/intfake.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/intfake.o bios/intfake.bin

bios/c-int10.o: bios/c-int10.c
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS) -c bios/c-int10.c -o bios/c-int10.o

bios/c-int10.c: bios/int10.bin
	$(E) "  BIN2C   " $@
	$(Q) python bios/bin2c.py -i bios/int10.bin -o bios/c-int10.c -n int10

bios/int10.o: bios/int10.S
	$(E) "  CC      " $@
	$(Q) $(CC) $(CFLAGS)  $(BIOS) -c -o bios/int10.o bios/int10.S

bios/int10.bin: bios/int10.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/int10.o bios/int10.bin

clean:
	$(E) "  CLEAN"
	$(Q) rm -f bios/*.bin
	$(Q) rm -f bios/*.o
	$(Q) rm -f bios/*.c
	$(Q) rm -f $(OBJS) $(PROGRAM)
.PHONY: clean
