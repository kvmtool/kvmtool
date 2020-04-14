#ifndef KVM__PCI_H
#define KVM__PCI_H

#include <linux/types.h>
#include <linux/kvm.h>
#include <linux/pci_regs.h>
#include <endian.h>

#include "kvm/devices.h"
#include "kvm/msi.h"
#include "kvm/fdt.h"

/*
 * PCI Configuration Mechanism #1 I/O ports. See Section 3.7.4.1.
 * ("Configuration Mechanism #1") of the PCI Local Bus Specification 2.1 for
 * details.
 */
#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc
#define PCI_CONFIG_BUS_FORWARD	0xcfa
#define PCI_IO_SIZE		0x100
#define PCI_IOPORT_START	0x6200
#define PCI_CFG_SIZE		(1ULL << 24)

struct kvm;

union pci_config_address {
	struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
		unsigned	reg_offset	: 2;		/* 1  .. 0  */
		unsigned	register_number	: 6;		/* 7  .. 2  */
		unsigned	function_number	: 3;		/* 10 .. 8  */
		unsigned	device_number	: 5;		/* 15 .. 11 */
		unsigned	bus_number	: 8;		/* 23 .. 16 */
		unsigned	reserved	: 7;		/* 30 .. 24 */
		unsigned	enable_bit	: 1;		/* 31       */
#else
		unsigned	enable_bit	: 1;		/* 31       */
		unsigned	reserved	: 7;		/* 30 .. 24 */
		unsigned	bus_number	: 8;		/* 23 .. 16 */
		unsigned	device_number	: 5;		/* 15 .. 11 */
		unsigned	function_number	: 3;		/* 10 .. 8  */
		unsigned	register_number	: 6;		/* 7  .. 2  */
		unsigned	reg_offset	: 2;		/* 1  .. 0  */
#endif
	};
	u32 w;
};

struct msix_table {
	struct msi_msg msg;
	u32 ctrl;
};

struct msix_cap {
	u8 cap;
	u8 next;
	u16 ctrl;
	u32 table_offset;
	u32 pba_offset;
};

struct msi_cap_64 {
	u8 cap;
	u8 next;
	u16 ctrl;
	u32 address_lo;
	u32 address_hi;
	u16 data;
	u16 _align;
	u32 mask_bits;
	u32 pend_bits;
};

struct msi_cap_32 {
	u8 cap;
	u8 next;
	u16 ctrl;
	u32 address_lo;
	u16 data;
	u16 _align;
	u32 mask_bits;
	u32 pend_bits;
};

struct pci_cap_hdr {
	u8	type;
	u8	next;
};

#define PCI_BAR_OFFSET(b)	(offsetof(struct pci_device_header, bar[b]))
#define PCI_DEV_CFG_SIZE	256
#define PCI_DEV_CFG_MASK	(PCI_DEV_CFG_SIZE - 1)

struct pci_device_header;

struct pci_config_operations {
	void (*write)(struct kvm *kvm, struct pci_device_header *pci_hdr,
		      u8 offset, void *data, int sz);
	void (*read)(struct kvm *kvm, struct pci_device_header *pci_hdr,
		     u8 offset, void *data, int sz);
};

struct pci_device_header {
	/* Configuration space, as seen by the guest */
	union {
		struct {
			u16		vendor_id;
			u16		device_id;
			u16		command;
			u16		status;
			u8		revision_id;
			u8		class[3];
			u8		cacheline_size;
			u8		latency_timer;
			u8		header_type;
			u8		bist;
			u32		bar[6];
			u32		card_bus;
			u16		subsys_vendor_id;
			u16		subsys_id;
			u32		exp_rom_bar;
			u8		capabilities;
			u8		reserved1[3];
			u32		reserved2;
			u8		irq_line;
			u8		irq_pin;
			u8		min_gnt;
			u8		max_lat;
			struct msix_cap msix;
		} __attribute__((packed));
		/* Pad to PCI config space size */
		u8	__pad[PCI_DEV_CFG_SIZE];
	};

	/* Private to lkvm */
	u32		bar_size[6];
	struct pci_config_operations	cfg_ops;
	/*
	 * PCI INTx# are level-triggered, but virtual device often feature
	 * edge-triggered INTx# for convenience.
	 */
	enum irq_type	irq_type;
};

#define PCI_CAP(pci_hdr, pos) ((void *)(pci_hdr) + (pos))

#define pci_for_each_cap(pos, cap, hdr)				\
	for ((pos) = (hdr)->capabilities & ~3;			\
	     (cap) = PCI_CAP(hdr, pos), (pos) != 0;		\
	     (pos) = ((struct pci_cap_hdr *)(cap))->next & ~3)

int pci__init(struct kvm *kvm);
int pci__exit(struct kvm *kvm);
struct pci_device_header *pci__find_dev(u8 dev_num);
u32 pci_get_mmio_block(u32 size);
u16 pci_get_io_port_block(u32 size);
void pci__assign_irq(struct device_header *dev_hdr);
void pci__config_wr(struct kvm *kvm, union pci_config_address addr, void *data, int size);
void pci__config_rd(struct kvm *kvm, union pci_config_address addr, void *data, int size);

void *pci_find_cap(struct pci_device_header *hdr, u8 cap_type);

#endif /* KVM__PCI_H */
