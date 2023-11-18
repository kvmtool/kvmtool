#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include <stdbool.h>
#include <linux/const.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define MAX_PAGE_SIZE		SZ_4K

#define RISCV_IOPORT		0x00000000ULL
#define RISCV_IOPORT_SIZE	SZ_64K
#define RISCV_IRQCHIP		0x08000000ULL
#define RISCV_IRQCHIP_SIZE	SZ_128M
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

#define RISCV_UART_MMIO_BASE	RISCV_MMIO
#define RISCV_UART_MMIO_SIZE	0x10000

#define RISCV_RTC_MMIO_BASE	(RISCV_UART_MMIO_BASE + RISCV_UART_MMIO_SIZE)
#define RISCV_RTC_MMIO_SIZE	0x10000

#define KVM_IOPORT_AREA		RISCV_IOPORT
#define KVM_PCI_CFG_AREA	RISCV_PCI
#define KVM_PCI_MMIO_AREA	(KVM_PCI_CFG_AREA + RISCV_PCI_CFG_SIZE)
#define KVM_VIRTIO_MMIO_AREA	(RISCV_RTC_MMIO_BASE + RISCV_RTC_MMIO_SIZE)

#define KVM_IOEVENTFD_HAS_PIO	0

#define KVM_IRQ_OFFSET		1

#define KVM_VM_TYPE		0

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

enum irqchip_type {
	IRQCHIP_UNKNOWN = 0,
	IRQCHIP_PLIC,
	IRQCHIP_AIA
};

extern enum irqchip_type riscv_irqchip;
extern bool riscv_irqchip_inkernel;
extern void (*riscv_irqchip_trigger)(struct kvm *kvm, int irq,
				     int level, bool edge);
extern void (*riscv_irqchip_generate_fdt_node)(void *fdt, struct kvm *kvm);
extern u32 riscv_irqchip_phandle;
extern u32 riscv_irqchip_msi_phandle;
extern bool riscv_irqchip_line_sensing;
extern bool riscv_irqchip_irqfd_ready;

void plic__create(struct kvm *kvm);

void pci__generate_fdt_nodes(void *fdt);

int riscv__add_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd,
		     int resample_fd);

void riscv__del_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd);

#define irq__add_irqfd riscv__add_irqfd
#define irq__del_irqfd riscv__del_irqfd

int riscv__setup_irqfd_lines(struct kvm *kvm);

void riscv__generate_irq_prop(void *fdt, u8 irq, enum irq_type irq_type);

void riscv__irqchip_create(struct kvm *kvm);

#endif /* KVM__KVM_ARCH_H */
