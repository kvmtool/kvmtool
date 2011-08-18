#
# Define WERROR=0 to disable -Werror.
#

ifeq ($(strip $(V)),)
	E = @echo
	Q = @
else
	E = @\#
	Q =
endif
export E Q

include config/utilities.mak
include config/feature-tests.mak

FIND	:= find
CSCOPE	:= cscope
TAGS	:= ctags

PROGRAM	:= kvm

OBJS	+= builtin-balloon.o
OBJS	+= builtin-debug.o
OBJS	+= builtin-help.o
OBJS	+= builtin-list.o
OBJS	+= builtin-stat.o
OBJS	+= builtin-pause.o
OBJS	+= builtin-resume.o
OBJS	+= builtin-run.o
OBJS	+= builtin-stop.o
OBJS	+= builtin-version.o
OBJS	+= cpuid.o
OBJS	+= disk/core.o
OBJS	+= framebuffer.o
OBJS	+= guest_compat.o
OBJS	+= hw/rtc.o
OBJS	+= hw/serial.o
OBJS	+= interrupt.o
OBJS	+= ioport.o
OBJS	+= kvm-cpu.o
OBJS	+= kvm.o
OBJS	+= main.o
OBJS	+= mmio.o
OBJS	+= pci.o
OBJS	+= read-write.o
OBJS	+= term.o
OBJS	+= util.o
OBJS	+= virtio/blk.o
OBJS	+= virtio/console.o
OBJS	+= virtio/core.o
OBJS	+= virtio/net.o
OBJS	+= virtio/rng.o
OBJS    += virtio/balloon.o
OBJS	+= disk/blk.o
OBJS	+= disk/qcow.o
OBJS	+= disk/raw.o
OBJS	+= ioeventfd.o
OBJS	+= irq.o
OBJS	+= net/uip/core.o
OBJS	+= net/uip/arp.o
OBJS	+= net/uip/icmp.o
OBJS	+= net/uip/ipv4.o
OBJS	+= net/uip/tcp.o
OBJS	+= net/uip/udp.o
OBJS	+= net/uip/buf.o
OBJS	+= net/uip/csum.o
OBJS	+= net/uip/dhcp.o
OBJS	+= kvm-cmd.o
OBJS	+= mptable.o
OBJS	+= rbtree.o
OBJS	+= threadpool.o
OBJS	+= util/parse-options.o
OBJS	+= util/rbtree-interval.o
OBJS	+= util/strbuf.o
OBJS	+= virtio/9p.o
OBJS	+= virtio/9p-pdu.o
OBJS	+= hw/vesa.o
OBJS	+= hw/i8042.o

FLAGS_BFD := $(CFLAGS) -lbfd
has_bfd := $(call try-cc,$(SOURCE_BFD),$(FLAGS_BFD))
ifeq ($(has_bfd),y)
	CFLAGS	+= -DCONFIG_HAS_BFD
	OBJS	+= symbol.o
	LIBS	+= -lbfd
endif

FLAGS_VNCSERVER := $(CFLAGS) -lvncserver
has_vncserver := $(call try-cc,$(SOURCE_VNCSERVER),$(FLAGS_VNCSERVER))
ifeq ($(has_vncserver),y)
	OBJS	+= ui/vnc.o
	CFLAGS	+= -DCONFIG_HAS_VNCSERVER
	LIBS	+= -lvncserver
endif

FLAGS_SDL := $(CFLAGS) -lSDL
has_SDL := $(call try-cc,$(SOURCE_SDL),$(FLAGS_SDL))
ifeq ($(has_SDL),y)
	OBJS	+= ui/sdl.o
	CFLAGS	+= -DCONFIG_HAS_SDL
	LIBS	+= -lSDL
endif

DEPS	:= $(patsubst %.o,%.d,$(OBJS))

# Exclude BIOS object files from header dependencies.
OBJS	+= bios.o
OBJS	+= bios/bios-rom.o

LIBS	+= -lrt
LIBS	+= -lpthread

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
DEFINES	+= -DKVMTOOLS_VERSION='"$(KVMTOOLS_VERSION)"'

KVM_INCLUDE := include
CFLAGS	+= $(CPPFLAGS) $(DEFINES) -I$(KVM_INCLUDE) -I../../include -I../../arch/$(ARCH)/include/ -Os -g

ifneq ($(WERROR),0)
	WARNINGS += -Werror
endif

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
WARNINGS += -Wunused-result

CFLAGS	+= $(WARNINGS)

all: $(PROGRAM)

KVMTOOLS-VERSION-FILE:
	@$(SHELL_PATH) util/KVMTOOLS-VERSION-GEN $(OUTPUT)
-include $(OUTPUT)KVMTOOLS-VERSION-FILE

$(PROGRAM): $(DEPS) $(OBJS)
	$(E) "  LINK    " $@
	$(Q) $(CC) $(OBJS) $(LIBS) -o $@

$(DEPS):

%.d: %.c
	$(Q) $(CC) -M -MT $(patsubst %.d,%.o,$@) $(CFLAGS) $< -o $@

# The header file common-cmds.h is needed for compilation of builtin-help.c.
builtin-help.d: $(KVM_INCLUDE)/common-cmds.h

$(OBJS):

rbtree.o: ../../lib/rbtree.c
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

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

bios.o: bios/bios.bin bios/bios-rom.h

bios/bios.bin.elf: bios/entry.S bios/e820.c bios/int10.c bios/int15.c bios/rom.ld.S
	$(E) "  CC       bios/e820.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/e820.c -o bios/e820.o
	$(E) "  CC       bios/int10.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/int10.c -o bios/int10.o
	$(E) "  CC       bios/int15.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/int15.c -o bios/int15.o
	$(E) "  CC       bios/entry.o"
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c -s bios/entry.S -o bios/entry.o
	$(E) "  LD      " $@
	$(Q) ld -T bios/rom.ld.S -o bios/bios.bin.elf bios/entry.o bios/e820.o bios/int10.o bios/int15.o

bios/bios.bin: bios/bios.bin.elf
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text bios/bios.bin.elf bios/bios.bin

bios/bios-rom.o: bios/bios-rom.S bios/bios.bin bios/bios-rom.h
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) bios/bios-rom.S -o bios/bios-rom.o

bios/bios-rom.h: bios/bios.bin.elf
	$(E) "  NM      " $@
	$(Q) cd bios && sh gen-offsets.sh > bios-rom.h && cd ..

check: $(PROGRAM)
	$(MAKE) -C tests
	./$(PROGRAM) run tests/pit/tick.bin
	./$(PROGRAM) run -d tests/boot/boot_test.iso -p "init=init"
.PHONY: check

clean:
	$(E) "  CLEAN"
	$(Q) rm -f bios/*.bin
	$(Q) rm -f bios/*.elf
	$(Q) rm -f bios/*.o
	$(Q) rm -f bios/bios-rom.h
	$(Q) rm -f tests/boot/boot_test.iso
	$(Q) rm -rf tests/boot/rootfs/
	$(Q) rm -f $(DEPS) $(OBJS) $(PROGRAM)
	$(Q) rm -f cscope.*
	$(Q) rm -f $(KVM_INCLUDE)/common-cmds.h
	$(Q) rm -f KVMTOOLS-VERSION-FILE
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
