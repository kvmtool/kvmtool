PROGRAM	= kvm

OBJS	+= cpuid.o
OBJS	+= interrupt.o
OBJS	+= ioport.o
OBJS	+= kvm.o
OBJS	+= main.o
OBJS	+= util.o
OBJS	+= bios/c-intfake.o
OBJS	+= bios/c-int10.o

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
	$(CC) $(OBJS) -o $@

$(OBJS):

#
# BIOS assembly weirdness
#
BIOS=-D__ASSEMBLY__ -m32 -march=i386 -Os -fno-strict-aliasing -fomit-frame-pointer
bios/c-intfake.o: bios/c-intfake.c
	$(CC) $(CFLAGS) -c bios/c-intfake.c -o bios/c-intfake.o
bios/c-intfake.c: bios/intfake.bin
	python bios/bin2c.py -i bios/intfake.bin -o bios/c-intfake.c -n intfake
bios/intfake.bin: bios/intfake.S
	$(CC) $(CFLAGS)  $(BIOS) -c -o bios/intfake.o bios/intfake.S
	objcopy -O binary -j .text bios/intfake.o bios/intfake.bin

bios/c-int10.o: bios/c-int10.c
	$(CC) $(CFLAGS) -c bios/c-int10.c -o bios/c-int10.o
bios/c-int10.c: bios/int10.bin
	python bios/bin2c.py -i bios/int10.bin -o bios/c-int10.c -n int10
bios/int10.bin: bios/int10.S
	$(CC) $(CFLAGS)  $(BIOS) -c -o bios/int10.o bios/int10.S
	objcopy -O binary -j .text bios/int10.o bios/int10.bin


clean:
	rm -f bios/*.bin
	rm -f bios/*.o
	rm -f bios/*.c
	rm -f $(OBJS) $(PROGRAM)
.PHONY: clean
