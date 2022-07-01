#ifndef KVM__PCI_H
#define KVM__PCI_H

#include <linux/types.h>
#include <linux/kvm.h>
#include <linux/pci_regs.h>
#include <linux/virtio_pci.h>
#include <endian.h>
#include <stdbool.h>

#include "kvm/devices.h"
#include "kvm/msi.h"
#include "kvm/fdt.h"
#include "kvm/kvm-arch.h"

#define pci_dev_err(pci_hdr, fmt, ...) \
	pr_err("[%04x:%04x] " fmt, (pci_hdr)->vendor_id, (pci_hdr)->device_id, ##__VA_ARGS__)
#define pci_dev_warn(pci_hdr, fmt, ...) \
	pr_warning("[%04x:%04x] " fmt, (pci_hdr)->vendor_id, (pci_hdr)->device_id, ##__VA_ARGS__)
#define pci_dev_info(pci_hdr, fmt, ...) \
	pr_info("[%04x:%04x] " fmt, (pci_hdr)->vendor_id, (pci_hdr)->device_id, ##__VA_ARGS__)
#define pci_dev_dbg(pci_hdr, fmt, ...) \
	pr_debug("[%04x:%04x] " fmt, (pci_hdr)->vendor_id, (pci_hdr)->device_id, ##__VA_ARGS__)
#define pci_dev_die(pci_hdr, fmt, ...) \
	die("[%04x:%04x] " fmt, (pci_hdr)->vendor_id, (pci_hdr)->device_id, ##__VA_ARGS__)

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

struct kvm;

/*
 * On some distributions, pci_regs.h doesn't define PCI_CFG_SPACE_SIZE and
 * PCI_CFG_SPACE_EXP_SIZE, so we define our own.
 */
#define PCI_CFG_SIZE_LEGACY		(1ULL << 24)
#define PCI_DEV_CFG_SIZE_LEGACY		256
#define PCI_CFG_SIZE_EXTENDED		(1ULL << 28)
#define PCI_DEV_CFG_SIZE_EXTENDED 	4096

#ifdef ARCH_HAS_PCI_EXP
#define arch_has_pci_exp()	(true)

#define PCI_CFG_SIZE		PCI_CFG_SIZE_EXTENDED
#define PCI_DEV_CFG_SIZE	PCI_DEV_CFG_SIZE_EXTENDED

union pci_config_address {
	struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
		unsigned	reg_offset	: 2;		/* 1  .. 0  */
		unsigned	register_number	: 10;		/* 11 .. 2  */
		unsigned	function_number	: 3;		/* 14 .. 12 */
		unsigned	device_number	: 5;		/* 19 .. 15 */
		unsigned	bus_number	: 8;		/* 27 .. 20 */
		unsigned	reserved	: 3;		/* 30 .. 28 */
		unsigned	enable_bit	: 1;		/* 31       */
#else
		unsigned	enable_bit	: 1;		/* 31       */
		unsigned	reserved	: 3;		/* 30 .. 28 */
		unsigned	bus_number	: 8;		/* 27 .. 20 */
		unsigned	device_number	: 5;		/* 19 .. 15 */
		unsigned	function_number	: 3;		/* 14 .. 12 */
		unsigned	register_number	: 10;		/* 11 .. 2  */
		unsigned	reg_offset	: 2;		/* 1  .. 0  */
#endif
	};
	u32 w;
};

#else
#define arch_has_pci_exp()	(false)

#define PCI_CFG_SIZE		PCI_CFG_SIZE_LEGACY
#define PCI_DEV_CFG_SIZE	PCI_DEV_CFG_SIZE_LEGACY

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
#endif /* ARCH_HAS_PCI_EXP */

#define PCI_DEV_CFG_MASK	(PCI_DEV_CFG_SIZE - 1)

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

struct virtio_caps {
	struct virtio_pci_cap		common;
	struct virtio_pci_notify_cap	notify;
	struct virtio_pci_cap		isr;
	struct virtio_pci_cap		device;
	struct virtio_pci_cfg_cap	pci;
};

struct pci_cap_hdr {
	u8	type;
	u8	next;
};

struct pci_exp_cap {
	u8 cap;
	u8 next;
	u16 cap_reg;
	u32 dev_cap;
	u16 dev_ctrl;
	u16 dev_status;
	u32 link_cap;
	u16 link_ctrl;
	u16 link_status;
	u32 slot_cap;
	u16 slot_ctrl;
	u16 slot_status;
	u16 root_ctrl;
	u16 root_cap;
	u32 root_status;
};

struct pci_device_header;

typedef int (*bar_activate_fn_t)(struct kvm *kvm,
				 struct pci_device_header *pci_hdr,
				 int bar_num, void *data);
typedef int (*bar_deactivate_fn_t)(struct kvm *kvm,
				   struct pci_device_header *pci_hdr,
				   int bar_num, void *data);

#define PCI_BAR_OFFSET(b)	(offsetof(struct pci_device_header, bar[b]))

struct pci_config_operations {
	void (*write)(struct kvm *kvm, struct pci_device_header *pci_hdr,
		      u16 offset, void *data, int sz);
	void (*read)(struct kvm *kvm, struct pci_device_header *pci_hdr,
		     u16 offset, void *data, int sz);
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
			/* Used only by architectures which support PCIE */
			struct pci_exp_cap pci_exp;
			struct virtio_caps virtio;
		} __attribute__((packed));
		/* Pad to PCI config space size */
		u8	__pad[PCI_DEV_CFG_SIZE];
	};

	/* Private to lkvm */
	u32			bar_size[6];
	bool			bar_active[6];
	bar_activate_fn_t	bar_activate_fn;
	bar_deactivate_fn_t	bar_deactivate_fn;
	void *data;
	struct pci_config_operations	cfg_ops;
	/*
	 * PCI INTx# are level-triggered, but virtual device often feature
	 * edge-triggered INTx# for convenience.
	 */
	enum irq_type	irq_type;
};

#define PCI_CAP(pci_hdr, pos) ((void *)(pci_hdr) + (pos))
#define PCI_CAP_OFF(pci_hdr, cap) ((void *)&(pci_hdr)->cap - (void *)(pci_hdr))

#define pci_for_each_cap(pos, cap, hdr)				\
	for ((pos) = (hdr)->capabilities & ~3;			\
	     (cap) = PCI_CAP(hdr, pos), (pos) != 0;		\
	     (pos) = ((struct pci_cap_hdr *)(cap))->next & ~3)

int pci__init(struct kvm *kvm);
int pci__exit(struct kvm *kvm);
struct pci_device_header *pci__find_dev(u8 dev_num);
u32 pci_get_mmio_block(u32 size);
u16 pci_get_io_port_block(u32 size);
int pci__assign_irq(struct pci_device_header *pci_hdr);
void pci__config_wr(struct kvm *kvm, union pci_config_address addr, void *data, int size);
void pci__config_rd(struct kvm *kvm, union pci_config_address addr, void *data, int size);

void *pci_find_cap(struct pci_device_header *hdr, u8 cap_type);

int pci__register_bar_regions(struct kvm *kvm, struct pci_device_header *pci_hdr,
			      bar_activate_fn_t bar_activate_fn,
			      bar_deactivate_fn_t bar_deactivate_fn, void *data);

static inline bool __pci__memory_space_enabled(u16 command)
{
	return command & PCI_COMMAND_MEMORY;
}

static inline bool pci__memory_space_enabled(struct pci_device_header *pci_hdr)
{
	return __pci__memory_space_enabled(pci_hdr->command);
}

static inline bool __pci__io_space_enabled(u16 command)
{
	return command & PCI_COMMAND_IO;
}

static inline bool pci__io_space_enabled(struct pci_device_header *pci_hdr)
{
	return __pci__io_space_enabled(pci_hdr->command);
}

static inline bool __pci__bar_is_io(u32 bar)
{
	return bar & PCI_BASE_ADDRESS_SPACE_IO;
}

static inline bool pci__bar_is_io(struct pci_device_header *pci_hdr, int bar_num)
{
	return __pci__bar_is_io(pci_hdr->bar[bar_num]);
}

static inline bool pci__bar_is_memory(struct pci_device_header *pci_hdr, int bar_num)
{
	return !pci__bar_is_io(pci_hdr, bar_num);
}

static inline u32 __pci__bar_address(u32 bar)
{
	if (__pci__bar_is_io(bar))
		return bar & PCI_BASE_ADDRESS_IO_MASK;
	return bar & PCI_BASE_ADDRESS_MEM_MASK;
}

static inline u32 pci__bar_address(struct pci_device_header *pci_hdr, int bar_num)
{
	return __pci__bar_address(pci_hdr->bar[bar_num]);
}

static inline u32 pci__bar_size(struct pci_device_header *pci_hdr, int bar_num)
{
	return pci_hdr->bar_size[bar_num];
}

#endif /* KVM__PCI_H */
