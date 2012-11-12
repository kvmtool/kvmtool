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

CC	:= $(CROSS_COMPILE)gcc
LD	:= $(CROSS_COMPILE)ld

FIND	:= find
CSCOPE	:= cscope
TAGS	:= ctags
INSTALL := install

prefix = $(HOME)
bindir_relative = bin
bindir = $(prefix)/$(bindir_relative)

DESTDIR_SQ = $(subst ','\'',$(DESTDIR))
bindir_SQ = $(subst ','\'',$(bindir))

PROGRAM	:= lkvm
PROGRAM_ALIAS := vm

GUEST_INIT := guest/init

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
OBJS	+= virtio/scsi.o
OBJS	+= virtio/console.o
OBJS	+= virtio/core.o
OBJS	+= virtio/net.o
OBJS	+= virtio/rng.o
OBJS    += virtio/balloon.o
OBJS	+= virtio/pci.o
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
OBJS	+= util/init.o
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
OBJS	+= virtio/mmio.o

# Translate uname -m into ARCH string
ARCH ?= $(shell uname -m | sed -e s/i.86/i386/ -e s/ppc.*/powerpc/)

ifeq ($(ARCH),i386)
	ARCH         := x86
	DEFINES      += -DCONFIG_X86_32
endif
ifeq ($(ARCH),x86_64)
	ARCH         := x86
	DEFINES      += -DCONFIG_X86_64
endif

LIBFDT_SRC = fdt.o fdt_ro.o fdt_wip.o fdt_sw.o fdt_rw.o fdt_strerror.o
LIBFDT_OBJS = $(patsubst %,../../scripts/dtc/libfdt/%,$(LIBFDT_SRC))

### Arch-specific stuff

#x86
ifeq ($(ARCH),x86)
	DEFINES += -DCONFIG_X86
	OBJS	+= x86/boot.o
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
ifeq ($(ARCH), powerpc)
	DEFINES += -DCONFIG_PPC
	OBJS	+= powerpc/boot.o
	OBJS	+= powerpc/ioport.o
	OBJS	+= powerpc/irq.o
	OBJS	+= powerpc/kvm.o
	OBJS	+= powerpc/cpu_info.o
	OBJS	+= powerpc/kvm-cpu.o
	OBJS	+= powerpc/spapr_hcall.o
	OBJS	+= powerpc/spapr_rtas.o
	OBJS	+= powerpc/spapr_hvcons.o
	OBJS	+= powerpc/spapr_pci.o
	OBJS	+= powerpc/xics.o
# We use libfdt, but it's sometimes not packaged 64bit.  It's small too,
# so just build it in:
	CFLAGS 	+= -I../../scripts/dtc/libfdt
	OTHEROBJS	+= $(LIBFDT_OBJS)
	ARCH_INCLUDE := powerpc/include
	CFLAGS 	+= -m64
endif

###

ifeq (,$(ARCH_INCLUDE))
	UNSUPP_ERR = @echo "This architecture is not supported in kvmtool." && exit 1
else
	UNSUPP_ERR =
endif

###

# Detect optional features.
# On a given system, some libs may link statically, some may not; so, check
# both and only build those that link!

FLAGS_BFD := $(CFLAGS) -lbfd
ifeq ($(call try-cc,$(SOURCE_BFD),$(FLAGS_BFD) -static),y)
	CFLAGS_STATOPT	+= -DCONFIG_HAS_BFD
	OBJS_STATOPT	+= symbol.o
	LIBS_STATOPT	+= -lbfd
endif

FLAGS_VNCSERVER := $(CFLAGS) -lvncserver
ifeq ($(call try-cc,$(SOURCE_VNCSERVER),$(FLAGS_VNCSERVER)),y)
	OBJS_DYNOPT	+= ui/vnc.o
	CFLAGS_DYNOPT	+= -DCONFIG_HAS_VNCSERVER
	LIBS_DYNOPT	+= -lvncserver
endif
ifeq ($(call try-cc,$(SOURCE_VNCSERVER),$(FLAGS_VNCSERVER) -static),y)
	OBJS_STATOPT	+= ui/vnc.o
	CFLAGS_STATOPT	+= -DCONFIG_HAS_VNCSERVER
	LIBS_STATOPT	+= -lvncserver
endif

FLAGS_SDL := $(CFLAGS) -lSDL
ifeq ($(call try-cc,$(SOURCE_SDL),$(FLAGS_SDL)),y)
	OBJS_DYNOPT	+= ui/sdl.o
	CFLAGS_DYNOPT	+= -DCONFIG_HAS_SDL
	LIBS_DYNOPT	+= -lSDL
endif
ifeq ($(call try-cc,$(SOURCE_SDL),$(FLAGS_SDL) -static), y)
	OBJS_STATOPT	+= ui/sdl.o
	CFLAGS_STATOPT	+= -DCONFIG_HAS_SDL
	LIBS_STATOPT	+= -lSDL
endif

FLAGS_ZLIB := $(CFLAGS) -lz
ifeq ($(call try-cc,$(SOURCE_ZLIB),$(FLAGS_ZLIB)),y)
	CFLAGS_DYNOPT	+= -DCONFIG_HAS_ZLIB
	LIBS_DYNOPT	+= -lz
endif
ifeq ($(call try-cc,$(SOURCE_ZLIB),$(FLAGS_ZLIB) -static),y)
	CFLAGS_STATOPT	+= -DCONFIG_HAS_ZLIB
	LIBS_STATOPT	+= -lz
endif

FLAGS_AIO := $(CFLAGS) -laio
ifeq ($(call try-cc,$(SOURCE_AIO),$(FLAGS_AIO)),y)
	CFLAGS_DYNOPT	+= -DCONFIG_HAS_AIO
	LIBS_DYNOPT	+= -laio
endif
ifeq ($(call try-cc,$(SOURCE_AIO),$(FLAGS_AIO) -static),y)
	CFLAGS_STATOPT	+= -DCONFIG_HAS_AIO
	LIBS_STATOPT	+= -laio
endif

FLAGS_LTO := -flto
ifeq ($(call try-cc,$(SOURCE_HELLO),$(FLAGS_LTO)),y)
	CFLAGS		+= $(FLAGS_LTO)
endif

ifneq ($(call try-build,$(SOURCE_STATIC),-static,),y)
$(error No static libc found. Please install glibc-static package.)
endif
###

LIBS	+= -lrt
LIBS	+= -lpthread
LIBS	+= -lutil


DEPS	:= $(patsubst %.o,%.d,$(OBJS))

DEFINES	+= -D_FILE_OFFSET_BITS=64
DEFINES	+= -D_GNU_SOURCE
DEFINES	+= -DKVMTOOLS_VERSION='"$(KVMTOOLS_VERSION)"'
DEFINES	+= -DBUILD_ARCH='"$(ARCH)"'

KVM_INCLUDE := include
CFLAGS	+= $(CPPFLAGS) $(DEFINES) -I$(KVM_INCLUDE) -I$(ARCH_INCLUDE) -I$(KINCL_PATH)/include/uapi -I$(KINCL_PATH)/include -I$(KINCL_PATH)/arch/$(ARCH)/include/uapi -I$(KINCL_PATH)/arch/$(ARCH)/include/ -O2 -fno-strict-aliasing -g

WARNINGS += -Wall
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

# Some targets may use 'external' sources that don't build totally cleanly.
CFLAGS_EASYGOING := $(CFLAGS)

ifneq ($(WERROR),0)
	CFLAGS += -Werror
endif

all: arch_support_check $(PROGRAM) $(PROGRAM_ALIAS) $(GUEST_INIT)

arch_support_check:
	$(UNSUPP_ERR)

KVMTOOLS-VERSION-FILE:
	@$(SHELL_PATH) util/KVMTOOLS-VERSION-GEN $(OUTPUT)
-include $(OUTPUT)KVMTOOLS-VERSION-FILE

# When building -static all objects are built with appropriate flags, which
# may differ between static & dynamic .o.  The objects are separated into
# .o and .static.o.  See the %.o: %.c rules below.
#
# $(OTHEROBJS) are things that do not get substituted like this.
#
STATIC_OBJS = $(patsubst %.o,%.static.o,$(OBJS) $(OBJS_STATOPT))
GUEST_OBJS = guest/guest_init.o

$(PROGRAM)-static:  $(DEPS) $(STATIC_OBJS) $(OTHEROBJS) $(GUEST_INIT)
	$(E) "  LINK    " $@
	$(Q) $(CC) -static $(CFLAGS) $(STATIC_OBJS) $(OTHEROBJS) $(GUEST_OBJS) $(LIBS) $(LIBS_STATOPT) -o $@

$(PROGRAM): $(DEPS) $(OBJS) $(OBJS_DYNOPT) $(OTHEROBJS) $(GUEST_INIT)
	$(E) "  LINK    " $@
	$(Q) $(CC) $(CFLAGS) $(OBJS) $(OBJS_DYNOPT) $(OTHEROBJS) $(GUEST_OBJS) $(LIBS) $(LIBS_DYNOPT) -o $@

$(PROGRAM_ALIAS): $(PROGRAM)
	$(E) "  LN      " $@
	$(Q) ln -f $(PROGRAM) $@

$(GUEST_INIT): guest/init.c
	$(E) "  LINK    " $@
	$(Q) $(CC) -static guest/init.c -o $@
	$(Q) $(LD) -r -b binary -o guest/guest_init.o $(GUEST_INIT)

$(DEPS):

util/rbtree.d: ../../lib/rbtree.c
	$(Q) $(CC) -M -MT util/rbtree.o $(CFLAGS) $< -o $@

%.d: %.c
	$(Q) $(CC) -M -MT $(patsubst %.d,%.o,$@) $(CFLAGS) $< -o $@

%.s: %.c
	$(Q) $(CC) -o $@ -S $(CFLAGS) -fverbose-asm $<

# The header file common-cmds.h is needed for compilation of builtin-help.c.
builtin-help.d: $(KVM_INCLUDE)/common-cmds.h

$(OBJS):

# This rule relaxes the -Werror on libfdt, since for now it still has
# a bunch of warnings. :(
../../scripts/dtc/libfdt/%.o: ../../scripts/dtc/libfdt/%.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS_EASYGOING) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS_EASYGOING) $< -o $@

util/rbtree.static.o util/rbtree.o: ../../lib/rbtree.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

%.static.o: %.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS) $(CFLAGS_STATOPT) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $(CFLAGS_STATOPT)  $< -o $@

%.o: %.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS) $(CFLAGS_DYNOPT) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $(CFLAGS_DYNOPT) $< -o $@


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
	$(Q) $(LD) -T x86/bios/rom.ld.S -o x86/bios/bios.bin.elf x86/bios/memcpy.o x86/bios/entry.o x86/bios/e820.o x86/bios/int10.o x86/bios/int15.o

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

install: all
	$(E) "  INSTALL"
	$(Q) $(INSTALL) -d -m 755 '$(DESTDIR_SQ)$(bindir_SQ)' 
	$(Q) $(INSTALL) $(PROGRAM) '$(DESTDIR_SQ)$(bindir_SQ)' 
.PHONY: install

clean:
	$(E) "  CLEAN"
	$(Q) rm -f x86/bios/*.bin
	$(Q) rm -f x86/bios/*.elf
	$(Q) rm -f x86/bios/*.o
	$(Q) rm -f x86/bios/bios-rom.h
	$(Q) rm -f tests/boot/boot_test.iso
	$(Q) rm -rf tests/boot/rootfs/
	$(Q) rm -f $(DEPS) $(OBJS) $(OTHEROBJS) $(OBJS_DYNOPT) $(STATIC_OBJS) $(PROGRAM) $(PROGRAM_ALIAS) $(PROGRAM)-static $(GUEST_INIT) $(GUEST_OBJS)
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
