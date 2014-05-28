#ifndef ARM_COMMON__KVM_ARCH_H
#define ARM_COMMON__KVM_ARCH_H

#include <stdbool.h>
#include <linux/const.h>
#include <linux/types.h>

#include "arm-common/gic.h"

#define ARM_IOPORT_AREA		_AC(0x0000000000000000, UL)
#define ARM_MMIO_AREA		_AC(0x0000000000010000, UL)
#define ARM_AXI_AREA		_AC(0x0000000040000000, UL)
#define ARM_MEMORY_AREA		_AC(0x0000000080000000, UL)

#define ARM_LOMAP_MAX_MEMORY	((1ULL << 32) - ARM_MEMORY_AREA)
#define ARM_HIMAP_MAX_MEMORY	((1ULL << 40) - ARM_MEMORY_AREA)

#define ARM_GIC_DIST_BASE	(ARM_AXI_AREA - ARM_GIC_DIST_SIZE)
#define ARM_GIC_CPUI_BASE	(ARM_GIC_DIST_BASE - ARM_GIC_CPUI_SIZE)
#define ARM_GIC_SIZE		(ARM_GIC_DIST_SIZE + ARM_GIC_CPUI_SIZE)

#define ARM_IOPORT_SIZE		(ARM_MMIO_AREA - ARM_IOPORT_AREA)
#define ARM_VIRTIO_MMIO_SIZE	(ARM_AXI_AREA - (ARM_MMIO_AREA + ARM_GIC_SIZE))
#define ARM_PCI_CFG_SIZE	(1ULL << 24)
#define ARM_PCI_MMIO_SIZE	(ARM_MEMORY_AREA - \
				(ARM_AXI_AREA + ARM_PCI_CFG_SIZE))

#define KVM_IOPORT_AREA		ARM_IOPORT_AREA
#define KVM_PCI_CFG_AREA	ARM_AXI_AREA
#define KVM_PCI_MMIO_AREA	(KVM_PCI_CFG_AREA + ARM_PCI_CFG_SIZE)
#define KVM_VIRTIO_MMIO_AREA	ARM_MMIO_AREA

#define KVM_IRQ_OFFSET		GIC_SPI_IRQ_BASE

#define KVM_VM_TYPE		0

#define VIRTIO_DEFAULT_TRANS(kvm)	\
	((kvm)->cfg.arch.virtio_trans_pci ? VIRTIO_PCI : VIRTIO_MMIO)

#define VIRTIO_RING_ENDIAN	(VIRTIO_ENDIAN_LE | VIRTIO_ENDIAN_BE)

static inline bool arm_addr_in_ioport_region(u64 phys_addr)
{
	u64 limit = KVM_IOPORT_AREA + ARM_IOPORT_SIZE;
	return phys_addr >= KVM_IOPORT_AREA && phys_addr < limit;
}

static inline bool arm_addr_in_virtio_mmio_region(u64 phys_addr)
{
	u64 limit = KVM_VIRTIO_MMIO_AREA + ARM_VIRTIO_MMIO_SIZE;
	return phys_addr >= KVM_VIRTIO_MMIO_AREA && phys_addr < limit;
}

static inline bool arm_addr_in_pci_region(u64 phys_addr)
{
	u64 limit = KVM_PCI_CFG_AREA + ARM_PCI_CFG_SIZE + ARM_PCI_MMIO_SIZE;
	return phys_addr >= KVM_PCI_CFG_AREA && phys_addr < limit;
}

struct kvm_arch {
	/*
	 * We may have to align the guest memory for virtio, so keep the
	 * original pointers here for munmap.
	 */
	void	*ram_alloc_start;
	u64	ram_alloc_size;

	/*
	 * Guest addresses for memory layout.
	 */
	u64	memory_guest_start;
	u64	kern_guest_start;
	u64	initrd_guest_start;
	u64	initrd_size;
	u64	dtb_guest_start;
};

#endif /* ARM_COMMON__KVM_ARCH_H */
