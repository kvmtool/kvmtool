#ifndef KVM__PCI_H
#define KVM__PCI_H

#include <linux/types.h>
#include <linux/kvm.h>
#include <linux/pci_regs.h>
#include <endian.h>

#include "kvm/kvm.h"
#include "kvm/msi.h"

#define PCI_MAX_DEVICES		256
/*
 * PCI Configuration Mechanism #1 I/O ports. See Section 3.7.4.1.
 * ("Configuration Mechanism #1") of the PCI Local Bus Specification 2.1 for
 * details.
 */
#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc
#define PCI_CONFIG_BUS_FORWARD	0xcfa
#define PCI_IO_SIZE		0x100

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

struct pci_device_header {
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
	u8		empty[136]; /* Rest of PCI config space */
	u32		bar_size[6];
} __attribute__((packed));

int pci__init(struct kvm *kvm);
int pci__exit(struct kvm *kvm);
int pci__register(struct pci_device_header *dev, u8 dev_num);
struct pci_device_header *pci__find_dev(u8 dev_num);
u32 pci_get_io_space_block(u32 size);
void pci__config_wr(struct kvm *kvm, union pci_config_address addr, void *data, int size);
void pci__config_rd(struct kvm *kvm, union pci_config_address addr, void *data, int size);

#endif /* KVM__PCI_H */
