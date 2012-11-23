#ifndef ARM_COMMON__KVM_ARCH_H
#define ARM_COMMON__KVM_ARCH_H

#define VIRTIO_DEFAULT_TRANS	VIRTIO_MMIO

#ifndef __ASSEMBLY__

#include <stdbool.h>
#include <linux/types.h>

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
	u64	memory_guest_start;
	u64	kern_guest_start;
	u64	initrd_guest_start;
	u64	initrd_size;
	u64	dtb_guest_start;
	u64	smp_pen_guest_start;
	u64	smp_jump_guest_start;
};

#endif /* __ASSEMBLY__ */
#endif /* ARM_COMMON__KVM_ARCH_H */
