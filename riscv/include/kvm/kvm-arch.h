#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include <stdbool.h>
#include <linux/const.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define MAX_PAGE_SIZE		SZ_4K

#define RISCV_IOPORT		0x00000000ULL
#define RISCV_IOPORT_SIZE	SZ_64K
#define RISCV_PLIC		0x0c000000ULL
#define RISCV_PLIC_SIZE		SZ_64M
#define RISCV_MMIO		0x10000000ULL
#define RISCV_MMIO_SIZE		SZ_512M
#define RISCV_PCI		0x30000000ULL
/*
 * KVMTOOL emulates legacy PCI config space with 24bits device address
 * so 16M is sufficient but we reserve 256M to keep it future ready for
 * PCIe config space with 28bits device address.
 */
#define RISCV_PCI_CFG_SIZE	SZ_256M
#define RISCV_PCI_MMIO_SIZE	SZ_1G
#define RISCV_PCI_SIZE		(RISCV_PCI_CFG_SIZE + RISCV_PCI_MMIO_SIZE)

#define RISCV_RAM		0x80000000ULL

#define RISCV_LOMAP_MAX_MEMORY	((1ULL << 32) - RISCV_RAM)
#define RISCV_HIMAP_MAX_MEMORY	((1ULL << 40) - RISCV_RAM)

#if __riscv_xlen == 64
#define RISCV_MAX_MEMORY(kvm)	RISCV_HIMAP_MAX_MEMORY
#elif __riscv_xlen == 32
#define RISCV_MAX_MEMORY(kvm)	RISCV_LOMAP_MAX_MEMORY
#endif

#define KVM_IOPORT_AREA		RISCV_IOPORT
#define KVM_PCI_CFG_AREA	RISCV_PCI
#define KVM_PCI_MMIO_AREA	(KVM_PCI_CFG_AREA + RISCV_PCI_CFG_SIZE)
#define KVM_VIRTIO_MMIO_AREA	RISCV_MMIO

#define KVM_IOEVENTFD_HAS_PIO	0

#define KVM_IRQ_OFFSET		1

#define KVM_VM_TYPE		0

#define VIRTIO_DEFAULT_TRANS(kvm) \
	((kvm)->cfg.virtio_legacy ? VIRTIO_MMIO_LEGACY : VIRTIO_MMIO)

#define VIRTIO_RING_ENDIAN	VIRTIO_ENDIAN_LE

#define ARCH_HAS_PCI_EXP	1

struct kvm;

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

static inline bool riscv_addr_in_ioport_region(u64 phys_addr)
{
	u64 limit = KVM_IOPORT_AREA + RISCV_IOPORT_SIZE;
	return phys_addr >= KVM_IOPORT_AREA && phys_addr < limit;
}

enum irq_type;

void plic__generate_irq_prop(void *fdt, u8 irq, enum irq_type irq_type);

void plic__irq_trig(struct kvm *kvm, int irq, int level, bool edge);

void pci__generate_fdt_nodes(void *fdt);

#endif /* KVM__KVM_ARCH_H */
