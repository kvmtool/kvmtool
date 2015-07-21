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

CC	:= $(CROSS_COMPILE)gcc
CFLAGS	:=
LD	:= $(CROSS_COMPILE)ld
LDFLAGS	:=

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
OBJS	+= devices.o
OBJS	+= disk/core.o
OBJS	+= framebuffer.o
OBJS	+= guest_compat.o
OBJS	+= hw/rtc.o
OBJS	+= hw/serial.o
OBJS	+= ioport.o
OBJS	+= irq.o
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
OBJS    += util/iovec.o
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
OBJS	+= hw/i8042.o

# Translate uname -m into ARCH string
ARCH ?= $(shell uname -m | sed -e s/i.86/i386/ -e s/ppc.*/powerpc/ \
	  -e s/armv7.*/arm/ -e s/aarch64.*/arm64/ -e s/mips64/mips/)

ifeq ($(ARCH),i386)
	ARCH         := x86
	DEFINES      += -DCONFIG_X86_32
endif
ifeq ($(ARCH),x86_64)
	ARCH         := x86
	DEFINES      += -DCONFIG_X86_64
endif

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
# Exclude BIOS object files from header dependencies.
	OTHEROBJS	+= x86/bios.o
	OTHEROBJS	+= x86/bios/bios-rom.o
	ARCH_INCLUDE := x86/include
	ARCH_HAS_FRAMEBUFFER := y
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
	ARCH_INCLUDE := powerpc/include

	ARCH_WANT_LIBFDT := y
endif

# ARM
OBJS_ARM_COMMON		:= arm/fdt.o arm/gic.o arm/ioport.o arm/irq.o \
			   arm/kvm.o arm/kvm-cpu.o arm/pci.o arm/timer.o
HDRS_ARM_COMMON		:= arm/include
ifeq ($(ARCH), arm)
	DEFINES		+= -DCONFIG_ARM
	OBJS		+= $(OBJS_ARM_COMMON)
	OBJS		+= arm/aarch32/arm-cpu.o
	OBJS		+= arm/aarch32/kvm-cpu.o
	ARCH_INCLUDE	:= $(HDRS_ARM_COMMON)
	ARCH_INCLUDE	+= -Iarm/aarch32/include
	CFLAGS		+= -march=armv7-a

	ARCH_WANT_LIBFDT := y
endif

# ARM64
ifeq ($(ARCH), arm64)
	DEFINES		+= -DCONFIG_ARM64
	OBJS		+= $(OBJS_ARM_COMMON)
	OBJS		+= arm/aarch64/arm-cpu.o
	OBJS		+= arm/aarch64/kvm-cpu.o
	ARCH_INCLUDE	:= $(HDRS_ARM_COMMON)
	ARCH_INCLUDE	+= -Iarm/aarch64/include

	ARCH_WANT_LIBFDT := y
endif

ifeq ($(ARCH),mips)
	DEFINES		+= -DCONFIG_MIPS
	ARCH_INCLUDE	:= mips/include
	OBJS		+= mips/kvm.o
	OBJS		+= mips/kvm-cpu.o
	OBJS		+= mips/irq.o
endif
###

ifeq (,$(ARCH_INCLUDE))
        $(error This architecture ($(ARCH)) is not supported in kvmtool)
endif

###

# Detect optional features.
# On a given system, some libs may link statically, some may not; so, check
# both and only build those that link!

ifeq ($(call try-build,$(SOURCE_STRLCPY),$(CFLAGS),),y)
	CFLAGS_DYNOPT	+= -DHAVE_STRLCPY
	CFLAGS_STATOPT	+= -DHAVE_STRLCPY
endif

ifeq ($(call try-build,$(SOURCE_BFD),$(CFLAGS),-lbfd -static),y)
	CFLAGS_STATOPT	+= -DCONFIG_HAS_BFD
	OBJS_STATOPT	+= symbol.o
	LIBS_STATOPT	+= -lbfd
else
	NOTFOUND	+= bfd
endif

ifeq (y,$(ARCH_HAS_FRAMEBUFFER))
	CFLAGS_GTK3 := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
	LDFLAGS_GTK3 := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
	ifeq ($(call try-build,$(SOURCE_GTK3),$(CFLAGS) $(CFLAGS_GTK3),$(LDFLAGS_GTK3)),y)
		OBJS_DYNOPT	+= ui/gtk3.o
		CFLAGS_DYNOPT	+= -DCONFIG_HAS_GTK3 $(CFLAGS_GTK3)
		LIBS_DYNOPT	+= $(LDFLAGS_GTK3)
	else
		NOTFOUND	+= GTK3
	endif

	ifeq ($(call try-build,$(SOURCE_VNCSERVER),$(CFLAGS),-lvncserver),y)
		OBJS_DYNOPT	+= ui/vnc.o
		CFLAGS_DYNOPT	+= -DCONFIG_HAS_VNCSERVER
		LIBS_DYNOPT	+= -lvncserver
	else
		NOTFOUND	+= vncserver
	endif
	ifeq ($(call try-build,$(SOURCE_VNCSERVER),$(CFLAGS),-lvncserver -static),y)
		OBJS_STATOPT	+= ui/vnc.o
		CFLAGS_STATOPT	+= -DCONFIG_HAS_VNCSERVER
		LIBS_STATOPT	+= -lvncserver
	endif

	ifeq ($(call try-build,$(SOURCE_SDL),$(CFLAGS),-lSDL),y)
		OBJS_DYNOPT	+= ui/sdl.o
		CFLAGS_DYNOPT	+= -DCONFIG_HAS_SDL
		LIBS_DYNOPT	+= -lSDL
	else
		NOTFOUND	+= SDL
	endif
	ifeq ($(call try-build,$(SOURCE_SDL),$(CFLAGS),-lSDL -static), y)
		OBJS_STATOPT	+= ui/sdl.o
		CFLAGS_STATOPT	+= -DCONFIG_HAS_SDL
		LIBS_STATOPT	+= -lSDL
	endif
endif

ifeq ($(call try-build,$(SOURCE_ZLIB),$(CFLAGS),-lz),y)
	CFLAGS_DYNOPT	+= -DCONFIG_HAS_ZLIB
	LIBS_DYNOPT	+= -lz
else
	NOTFOUND	+= zlib
endif
ifeq ($(call try-build,$(SOURCE_ZLIB),$(CFLAGS),-lz -static),y)
	CFLAGS_STATOPT	+= -DCONFIG_HAS_ZLIB
	LIBS_STATOPT	+= -lz
endif

ifeq ($(call try-build,$(SOURCE_AIO),$(CFLAGS),-laio),y)
	CFLAGS_DYNOPT	+= -DCONFIG_HAS_AIO
	LIBS_DYNOPT	+= -laio
else
	NOTFOUND	+= aio
endif
ifeq ($(call try-build,$(SOURCE_AIO),$(CFLAGS),-laio -static),y)
	CFLAGS_STATOPT	+= -DCONFIG_HAS_AIO
	LIBS_STATOPT	+= -laio
endif

ifeq ($(LTO),1)
	FLAGS_LTO := -flto
	ifeq ($(call try-build,$(SOURCE_HELLO),$(CFLAGS),$(FLAGS_LTO)),y)
		CFLAGS		+= $(FLAGS_LTO)
	endif
endif

ifneq ($(call try-build,$(SOURCE_STATIC),,-static),y)
        $(error No static libc found. Please install glibc-static package.)
endif

ifeq (y,$(ARCH_WANT_LIBFDT))
	ifneq ($(call try-build,$(SOURCE_LIBFDT),$(CFLAGS),-lfdt),y)
          $(error No libfdt found. Please install libfdt-dev package)
	else
		CFLAGS_DYNOPT	+= -DCONFIG_HAS_LIBFDT
		CFLAGS_STATOPT	+= -DCONFIG_HAS_LIBFDT
		LIBS_DYNOPT	+= -lfdt
		LIBS_STATOPT	+= -lfdt
	endif
endif

ifneq ($(NOTFOUND),)
        $(warning Skipping optional libraries: $(NOTFOUND))
endif

###

LIBS	+= -lrt
LIBS	+= -lpthread
LIBS	+= -lutil


comma = ,

# The dependency file for the current target
depfile = $(subst $(comma),_,$(dir $@).$(notdir $@).d)

DEPS	:= $(foreach obj,$(OBJS),\
		$(subst $(comma),_,$(dir $(obj)).$(notdir $(obj)).d))

DEFINES	+= -D_FILE_OFFSET_BITS=64
DEFINES	+= -D_GNU_SOURCE
DEFINES	+= -DKVMTOOLS_VERSION='"$(KVMTOOLS_VERSION)"'
DEFINES	+= -DBUILD_ARCH='"$(ARCH)"'

KVM_INCLUDE := include
CFLAGS	+= $(CPPFLAGS) $(DEFINES) -I$(KVM_INCLUDE) -I$(ARCH_INCLUDE) -O2 -fno-strict-aliasing -g

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
WARNINGS += -Wno-format-nonliteral

CFLAGS	+= $(WARNINGS)

ifneq ($(WERROR),0)
	CFLAGS += -Werror
endif

all: $(PROGRAM) $(PROGRAM_ALIAS) $(GUEST_INIT)

# CFLAGS used when building objects
# This is intentionally not assigned using :=
c_flags	= -Wp,-MD,$(depfile) $(CFLAGS)

# When building -static all objects are built with appropriate flags, which
# may differ between static & dynamic .o.  The objects are separated into
# .o and .static.o.  See the %.o: %.c rules below.
#
# $(OTHEROBJS) are things that do not get substituted like this.
#
STATIC_OBJS = $(patsubst %.o,%.static.o,$(OBJS) $(OBJS_STATOPT))
GUEST_OBJS = guest/guest_init.o

$(PROGRAM)-static:  $(STATIC_OBJS) $(OTHEROBJS) $(GUEST_INIT)
	$(E) "  LINK    " $@
	$(Q) $(CC) -static $(CFLAGS) $(STATIC_OBJS) $(OTHEROBJS) $(GUEST_OBJS) $(LIBS) $(LIBS_STATOPT) -o $@

$(PROGRAM): $(OBJS) $(OBJS_DYNOPT) $(OTHEROBJS) $(GUEST_INIT)
	$(E) "  LINK    " $@
	$(Q) $(CC) $(CFLAGS) $(OBJS) $(OBJS_DYNOPT) $(OTHEROBJS) $(GUEST_OBJS) $(LIBS) $(LIBS_DYNOPT) -o $@

$(PROGRAM_ALIAS): $(PROGRAM)
	$(E) "  LN      " $@
	$(Q) ln -f $(PROGRAM) $@

$(GUEST_INIT): guest/init.c
	$(E) "  LINK    " $@
	$(Q) $(CC) -static guest/init.c -o $@
	$(Q) $(LD) $(LDFLAGS) -r -b binary -o guest/guest_init.o $(GUEST_INIT)

%.s: %.c
	$(Q) $(CC) -o $@ -S $(CFLAGS) -fverbose-asm $<

# The header file common-cmds.h is needed for compilation of builtin-help.c.
builtin-help.static.o builtin-help.o: $(KVM_INCLUDE)/common-cmds.h

$(OBJS):

util/rbtree.static.o util/rbtree.o: util/rbtree.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(c_flags) $< -o $@

%.static.o: %.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS) $(CFLAGS_STATOPT) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(c_flags) $(CFLAGS_STATOPT)  $< -o $@

%.o: %.c
ifeq ($(C),1)
	$(E) "  CHECK   " $@
	$(Q) $(CHECK) -c $(CFLAGS) $(CFLAGS_DYNOPT) $< -o $@
endif
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(c_flags) $(CFLAGS_DYNOPT) $< -o $@


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

x86/bios.o: x86/bios/bios.bin x86/bios/bios-rom.h

x86/bios/bios.bin.elf: x86/bios/entry.S x86/bios/e820.c x86/bios/int10.c x86/bios/int15.c x86/bios/rom.ld.S
	$(E) "  CC       x86/bios/memcpy.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/memcpy.c -o x86/bios/memcpy.o
	$(E) "  CC       x86/bios/e820.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/e820.c -o x86/bios/e820.o
	$(E) "  CC       x86/bios/int10.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/int10.c -o x86/bios/int10.o
	$(E) "  CC       x86/bios/int15.o"
	$(Q) $(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/int15.c -o x86/bios/int15.o
	$(E) "  CC       x86/bios/entry.o"
	$(Q) $(CC) $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/entry.S -o x86/bios/entry.o
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

#
# Escape redundant work on cleaning up
ifneq ($(MAKECMDGOALS),clean)
-include $(DEPS)

KVMTOOLS-VERSION-FILE:
	@$(SHELL_PATH) util/KVMTOOLS-VERSION-GEN $(OUTPUT)
-include $(OUTPUT)KVMTOOLS-VERSION-FILE
endif
