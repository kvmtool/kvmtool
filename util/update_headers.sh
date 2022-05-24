#!/bin/sh

########################################################################
# Updates the kvmtool tree with up-to-date public header files from
# a Linux source tree.
# If no directory is given on the command line, it will try to find one
# using the lib/modules/`uname -r`/source link.
########################################################################

set -ue

VIRTIO_LIST="virtio_9p.h virtio_balloon.h virtio_blk.h virtio_config.h \
	     virtio_console.h virtio_ids.h virtio_mmio.h virtio_net.h \
	     virtio_pci.h virtio_ring.h virtio_rng.h virtio_scsi.h \
	     virtio_vsock.h"

if [ "$#" -ge 1 ]
then
	LINUX_ROOT="$1"
else
	LINUX_ROOT="/lib/modules/$(uname -r)/source"
fi

if [ ! -d "$LINUX_ROOT/include/uapi/linux" ]
then
	echo "$LINUX_ROOT does not seem to be valid Linux source tree."
	echo "usage: $0 [path-to-Linux-source-tree]"
	exit 1
fi

cp -- "$LINUX_ROOT/include/uapi/linux/kvm.h" include/linux

for header in $VIRTIO_LIST
do
	cp -- "$LINUX_ROOT/include/uapi/linux/$header" include/linux
done

unset KVMTOOL_PATH

copy_optional_arch () {
	local src="$LINUX_ROOT/arch/$arch/include/uapi/$1"

	if [ -r "$src" ]
	then
		cp -- "$src" "$KVMTOOL_PATH/include/asm/"
	fi
}

for arch in arm64 mips powerpc riscv x86
do
	case "$arch" in
		arm64)	KVMTOOL_PATH=arm/aarch64
			copy_optional_arch asm/sve_context.h ;;
		*) KVMTOOL_PATH=$arch ;;
	esac
	cp -- "$LINUX_ROOT/arch/$arch/include/uapi/asm/kvm.h" \
		"$KVMTOOL_PATH/include/asm"
done
