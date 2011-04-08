ifeq ($(strip $(V)),)
	E = @echo
	Q = @
else
	E = @\#
	Q =
endif
export E Q

PROGRAM	= kvm
FIND = find
CSCOPE = cscope
TAGS = ctags

OBJS	+= 8250-serial.o
OBJS	+= virtio-blk.o
OBJS	+= virtio-console.o
OBJS	+= cpuid.o
OBJS	+= read-write.o
OBJS	+= disk-image.o
OBJS	+= interrupt.o
OBJS	+= ioport.o
OBJS	+= kvm.o
OBJS	+= main.o
OBJS	+= mmio.o
OBJS	+= pci.o
OBJS	+= util.o
OBJS	+= term.o
OBJS	+= virtio.o
OBJS	+= util/parse-options.o
OBJS	+= util/strbuf.o
OBJS	+= kvm-help.o
OBJS	+= kvm-cmd.o
OBJS	+= kvm-run.o

DEPS	:= $(patsubst %.o,%.d,$(OBJS))

# Exclude BIOS object files from header dependencies.
OBJS	+= bios.o
OBJS	+= bios/bios.o

LIBS	+= -lrt

# Additional ARCH settings for x86
ARCH ?= $(shell echo $(uname_M) | sed -e s/i.86/i386/ -e s/sun4u/sparc64/ \
                  -e s/arm.*/arm/ -e s/sa110/arm/ \
                  -e s/s390x/s390/ -e s/parisc64/parisc/ \
                  -e s/ppc.*/powerpc/ -e s/mips.*/mips/ \
                  -e s/sh[234].*/sh/ )

uname_M      := $(shell uname -m | sed -e s/i.86/i386/)
ifeq ($(uname_M),i386)
	ARCH         := x86
	DEFINES      += -DCONFIG_X86_32
endif
ifeq ($(uname_M),x86_64)
	ARCH         := x86
	DEFINES      += -DCONFIG_X86_64
endif

DEFINES	+= -D_FILE_OFFSET_BITS=64
DEFINES	+= -D_GNU_SOURCE

KVM_INCLUDE := include
CFLAGS	+= $(CPPFLAGS) $(DEFINES) -I$(KVM_INCLUDE) -I../../include -I../../arch/$(ARCH)/include/ -Os -g

WARNINGS += -Werror
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

$(PROGRAM): $(DEPS) $(OBJS)
	$(E) "  LINK    " $@
	$(Q) $(CC) $(OBJS) $(LIBS) -o $@

$(DEPS):

%.d: %.c
	$(Q) $(CC) -M -MT $(patsubst %.d,%.o,$@) $(CFLAGS) $< -o $@

# The header file common-cmds.h is needed for compilation of kvm-help.c.
kvm-help.d: $(KVM_INCLUDE)/common-cmds.h

$(OBJS):

%.o: %.c
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $< -o $@


$(KVM_INCLUDE)/common-cmds.h: util/generate-cmdlist.sh command-list.txt

$(KVM_INCLUDE)/common-cmds.h: $(wildcard Documentation/kvm-*.txt)
	$(E) "  GEN     " $@
	$(Q) util/generate-cmdlist.sh > $@+ && mv $@+ $@

#
# BIOS assembly weirdness
#
BIOS_CFLAGS += -m32
BIOS_CFLAGS += -march=i386
BIOS_CFLAGS += -mregparm=3

bios.o: bios/bios-rom.bin
bios/bios.o: bios/bios.S bios/bios-rom.bin
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) bios/bios.S -o bios/bios.o
	
bios/bios-rom.bin: bios/bios-rom.S bios/e820.c
	$(E) "  CC      " $@
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/e820.c -o bios/e820.o
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/bios-rom.S -o bios/bios-rom.o
	$(E) "  LD      " $@
	$(Q) ld -T bios/rom.ld.S -o bios/bios-rom.bin.elf bios/bios-rom.o bios/e820.o
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/bios-rom.bin.elf bios/bios-rom.bin
	$(E) "  NM      " $@
	$(Q) cd bios && sh gen-offsets.sh > bios-rom.h && cd ..

check: $(PROGRAM)
	$(MAKE) -C tests
	./$(PROGRAM) run tests/pit/tick.bin
.PHONY: check

clean:
	$(E) "  CLEAN"
	$(Q) rm -f bios/*.bin
	$(Q) rm -f bios/*.elf
	$(Q) rm -f bios/*.o
	$(Q) rm -f bios/bios-rom.h
	$(Q) rm -f $(DEPS) $(OBJS) $(PROGRAM)
	$(Q) rm -f cscope.*
	$(Q) rm -f $(KVM_INCLUDE)/common-cmds.h
.PHONY: clean

KVM_DEV	?= /dev/kvm

$(KVM_DEV):
	$(E) "  MKNOD " $@
	$(Q) mknod $@ char 10 232

devices: $(KVM_DEV)
.PHONY: devices

TAGS:
	$(E) "  GEN" $@
	$(Q) $(RM) -f TAGS
	$(Q) $(FIND) . -name '*.[hcS]' -print | xargs etags -a
.PHONY: TAGS

tags:
	$(E) "  GEN" $@
	$(Q) $(RM) -f tags
	$(Q) $(FIND) . -name '*.[hcS]' -print | xargs ctags -a
.PHONY: tags

cscope:
	$(E) "  GEN" $@
	$(Q) $(FIND) . -name '*.[hcS]' -print > cscope.files
	$(Q) $(CSCOPE) -bkqu
.PHONY: cscope

# Deps
-include $(DEPS)
