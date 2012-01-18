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
ifneq ($(I), )
	KINCL_PATH=$(I)
else
	KINCL_PATH=../..
endif
export E Q KINCL_PATH

include config/utilities.mak
include config/feature-tests.mak

FIND	:= find
CSCOPE	:= cscope
TAGS	:= ctags

PROGRAM	:= lkvm
PROGRAM_ALIAS := vm

GUEST_INIT := guest/init
GUEST_INIT_S2 := guest/init_stage2

OBJS	+= builtin-balloon.o
OBJS	+= builtin-debug.o
OBJS	+= builtin-help.o
OBJS	+= builtin-list.o
OBJS	+= builtin-stat.o
OBJS	+= builtin-pause.o
OBJS	+= builtin-resume.o
OBJS	+= builtin-run.o
OBJS	+= builtin-setup.o
OBJS	+= builtin-stop.o
OBJS	+= builtin-version.o
OBJS	+= disk/core.o
OBJS	+= framebuffer.o
OBJS	+= guest_compat.o
OBJS	+= hw/rtc.o
OBJS	+= hw/serial.o
OBJS	+= ioport.o
OBJS	+= kvm-cpu.o
OBJS	+= kvm.o
OBJS	+= main.o
OBJS	+= mmio.o
OBJS	+= pci.o
OBJS	+= term.o
OBJS	+= virtio/blk.o
OBJS	+= virtio/console.o
OBJS	+= virtio/core.o
OBJS	+= virtio/net.o
OBJS	+= virtio/rng.o
OBJS    += virtio/balloon.o
OBJS	+= virtio/pci.o
OBJS	+= virtio/trans.o
OBJS	+= disk/blk.o
OBJS	+= disk/qcow.o
OBJS	+= disk/raw.o
OBJS	+= ioeventfd.o
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
OBJS	+= util/rbtree.o
OBJS	+= util/threadpool.o
OBJS	+= util/parse-options.o
OBJS	+= util/rbtree-interval.o
OBJS	+= util/strbuf.o
OBJS	+= util/read-write.o
OBJS	+= util/util.o
OBJS	+= virtio/9p.o
OBJS	+= virtio/9p-pdu.o
OBJS	+= hw/vesa.o
OBJS	+= hw/pci-shmem.o
OBJS	+= kvm-ipc.o
OBJS	+= builtin-sandbox.o

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


### Arch-specific stuff

#x86
ifeq ($(ARCH),x86)
	DEFINES += -DCONFIG_X86
	OBJS	+= x86/cpuid.o
	OBJS	+= x86/interrupt.o
	OBJS	+= x86/ioport.o
	OBJS	+= x86/irq.o
	OBJS	+= x86/kvm.o
	OBJS	+= x86/kvm-cpu.o
	OBJS	+= x86/mptable.o
	OBJS	+= hw/i8042.o
# Exclude BIOS object files from header dependencies.
	OTHEROBJS	+= x86/bios.o
	OTHEROBJS	+= x86/bios/bios-rom.o
	ARCH_INCLUDE := x86/include
endif
# POWER/ppc:  Actually only support ppc64 currently.
ifeq ($(uname_M), ppc64)
	DEFINES += -DCONFIG_PPC
	OBJS	+= powerpc/ioport.o
	OBJS	+= powerpc/irq.o
	OBJS	+= powerpc/kvm.o
	OBJS	+= powerpc/kvm-cpu.o
	ARCH_INCLUDE := powerpc/include
	CFLAGS += -m64
endif

###

ifeq (,$(ARCH_INCLUDE))
	UNSUPP_ERR = @echo "This architecture is not supported in kvmtool." && exit 1
else
	UNSUPP_ERR =
endif


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

FLAGS_ZLIB := $(CFLAGS) -lz
has_ZLIB := $(call try-cc,$(SOURCE_ZLIB),$(FLAGS_ZLIB))
ifeq ($(has_ZLIB),y)
	CFLAGS	+= -DCONFIG_HAS_ZLIB
	LIBS	+= -lz
endif

FLAGS_AIO := $(CFLAGS) -laio
has_AIO := $(call try-cc,$(SOURCE_AIO),$(FLAGS_AIO))
ifeq ($(has_AIO),y)
	CFLAGS	+= -DCONFIG_HAS_AIO
	LIBS	+= -laio
endif

LIBS	+= -lrt
LIBS	+= -lpthread
LIBS	+= -lutil


DEPS	:= $(patsubst %.o,%.d,$(OBJS))
OBJS	+= $(OTHEROBJS)

DEFINES	+= -D_FILE_OFFSET_BITS=64
DEFINES	+= -D_GNU_SOURCE
DEFINES	+= -DKVMTOOLS_VERSION='"$(KVMTOOLS_VERSION)"'
DEFINES	+= -DBUILD_ARCH='"$(ARCH)"'

KVM_INCLUDE := include
CFLAGS	+= $(CPPFLAGS) $(DEFINES) -I$(KVM_INCLUDE) -I$(ARCH_INCLUDE) -I$(KINCL_PATH)/include -I$(KINCL_PATH)/arch/$(ARCH)/include/ -O2 -fno-strict-aliasing -g

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

CFLAGS	+= $(WARNINGS)

all: arch_support_check $(PROGRAM) $(PROGRAM_ALIAS) $(GUEST_INIT) $(GUEST_INIT_S2)

arch_support_check:
	$(UNSUPP_ERR)

KVMTOOLS-VERSION-FILE:
	@$(SHELL_PATH) util/KVMTOOLS-VERSION-GEN $(OUTPUT)
-include $(OUTPUT)KVMTOOLS-VERSION-FILE

$(PROGRAM): $(DEPS) $(OBJS)
	$(E) "  LINK    " $@
	$(Q) $(CC) $(CFLAGS) $(OBJS) $(LIBS) -o $@

$(PROGRAM_ALIAS): $(PROGRAM)
	$(E) "  LN      " $@
	$(Q) ln -f $(PROGRAM) $@

$(GUEST_INIT): guest/init.c
	$(E) "  LINK    " $@
	$(Q) $(CC) -static guest/init.c -o $@

$(GUEST_INIT_S2): guest/init_stage2.c
	$(E) "  LINK    " $@
	$(Q) $(CC) -static guest/init_stage2.c -o $@

$(DEPS):

util/rbtree.d: ../../lib/rbtree.c
	$(Q) $(CC) -M -MT util/rbtree.o $(CFLAGS) $< -o $@

%.d: %.c
	$(Q) $(CC) -M -MT $(patsubst %.d,%.o,$@) $(CFLAGS) $< -o $@

# The header file common-cmds.h is needed for compilation of builtin-help.c.
builtin-help.d: $(KVM_INCLUDE)/common-cmds.h

$(OBJS):

util/rbtree.o: ../../lib/rbtree.c
	$(E) "  CC      " $@
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

BIOS_CFLAGS += -fno-stack-protector
BIOS_CFLAGS += -I../../arch/$(ARCH)

x86/bios.o: x86/bios/bios.bin x86/bios/bios-rom.h

x86/bios/bios.bin.elf: x86/bios/entry.S x86/bios/e820.c x86/bios/int10.c x86/bios/int15.c x86/bios/rom.ld.S
	$(E) "  CC       x86/bios/memcpy.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s x86/bios/memcpy.c -o x86/bios/memcpy.o
	$(E) "  CC       x86/bios/e820.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s x86/bios/e820.c -o x86/bios/e820.o
	$(E) "  CC       x86/bios/int10.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s x86/bios/int10.c -o x86/bios/int10.o
	$(E) "  CC       x86/bios/int15.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c -s x86/bios/int15.c -o x86/bios/int15.o
	$(E) "  CC       x86/bios/entry.o"
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c -s x86/bios/entry.S -o x86/bios/entry.o
	$(E) "  LD      " $@
	$(Q) ld -T x86/bios/rom.ld.S -o x86/bios/bios.bin.elf x86/bios/memcpy.o x86/bios/entry.o x86/bios/e820.o x86/bios/int10.o x86/bios/int15.o

x86/bios/bios.bin: x86/bios/bios.bin.elf
	$(E) "  OBJCOPY " $@
	$(Q) objcopy -O binary -j .text x86/bios/bios.bin.elf x86/bios/bios.bin

x86/bios/bios-rom.o: x86/bios/bios-rom.S x86/bios/bios.bin x86/bios/bios-rom.h
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) x86/bios/bios-rom.S -o x86/bios/bios-rom.o

x86/bios/bios-rom.h: x86/bios/bios.bin.elf
	$(E) "  NM      " $@
	$(Q) cd x86/bios && sh gen-offsets.sh > bios-rom.h && cd ..

check: all
	$(MAKE) -C tests
	./$(PROGRAM) run tests/pit/tick.bin
	./$(PROGRAM) run -d tests/boot/boot_test.iso -p "init=init"
.PHONY: check

clean:
	$(E) "  CLEAN"
	$(Q) rm -f x86/bios/*.bin
	$(Q) rm -f x86/bios/*.elf
	$(Q) rm -f x86/bios/*.o
	$(Q) rm -f x86/bios/bios-rom.h
	$(Q) rm -f tests/boot/boot_test.iso
	$(Q) rm -rf tests/boot/rootfs/
	$(Q) rm -f $(DEPS) $(OBJS) $(PROGRAM) $(PROGRAM_ALIAS) $(GUEST_INIT) $(GUEST_INIT_S2)
	$(Q) rm -f cscope.*
	$(Q) rm -f tags
	$(Q) rm -f TAGS
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
