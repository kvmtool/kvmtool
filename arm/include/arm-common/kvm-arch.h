#ifndef ARM_COMMON__KVM_ARCH_H
#define ARM_COMMON__KVM_ARCH_H

#include <stdbool.h>
#include <linux/const.h>
#include <linux/types.h>

#define ARM_MMIO_AREA		_AC(0x0000000000000000, UL)
#define ARM_AXI_AREA		_AC(0x0000000040000000, UL)
#define ARM_MEMORY_AREA		_AC(0x0000000080000000, UL)

#define ARM_LOMAP_MAX_MEMORY	((1ULL << 32) - ARM_MEMORY_AREA)
#define ARM_HIMAP_MAX_MEMORY	((1ULL << 40) - ARM_MEMORY_AREA)

#define ARM_GIC_DIST_BASE	(ARM_AXI_AREA - ARM_GIC_DIST_SIZE)
#define ARM_GIC_CPUI_BASE	(ARM_GIC_DIST_BASE - ARM_GIC_CPUI_SIZE)
#define ARM_GIC_SIZE		(ARM_GIC_DIST_SIZE + ARM_GIC_CPUI_SIZE)

#define ARM_VIRTIO_MMIO_SIZE	(ARM_AXI_AREA - ARM_GIC_SIZE)
#define ARM_PCI_MMIO_SIZE	(ARM_MEMORY_AREA - ARM_AXI_AREA)

#define KVM_PCI_MMIO_AREA	ARM_AXI_AREA
#define KVM_VIRTIO_MMIO_AREA	ARM_MMIO_AREA

#define VIRTIO_DEFAULT_TRANS	VIRTIO_MMIO

static inline bool arm_addr_in_virtio_mmio_region(u64 phys_addr)
{
	u64 limit = KVM_VIRTIO_MMIO_AREA + ARM_VIRTIO_MMIO_SIZE;
	return phys_addr >= KVM_VIRTIO_MMIO_AREA && phys_addr < limit;
}

static inline bool arm_addr_in_pci_mmio_region(u64 phys_addr)
{
	u64 limit = KVM_PCI_MMIO_AREA + ARM_PCI_MMIO_SIZE;
	return phys_addr >= KVM_PCI_MMIO_AREA && phys_addr < limit;
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
