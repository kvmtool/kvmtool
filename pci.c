#include "kvm/pci.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <linux/err.h>
#include <assert.h>

#define PCI_BAR_OFFSET(b)		(offsetof(struct pci_device_header, bar[b]))

static struct pci_device_header		*pci_devices[PCI_MAX_DEVICES];

static union pci_config_address		pci_config_address;

/* This is within our PCI gap - in an unused area.
 * Note this is a PCI *bus address*, is used to assign BARs etc.!
 * (That's why it can still 32bit even with 64bit guests-- 64bit
 * PCI isn't currently supported.)
 */
static u32 io_space_blocks		= KVM_PCI_MMIO_AREA;

u32 pci_get_io_space_block(u32 size)
{
	u32 block = io_space_blocks;
	io_space_blocks += size;

	return block;
}

static void *pci_config_address_ptr(u16 port)
{
	unsigned long offset;
	void *base;

	offset	= port - PCI_CONFIG_ADDRESS;
	base	= &pci_config_address;

	return base + offset;
}

static bool pci_config_address_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	void *p = pci_config_address_ptr(port);

	memcpy(p, data, size);

	return true;
}

static bool pci_config_address_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	void *p = pci_config_address_ptr(port);

	memcpy(data, p, size);

	return true;
}

static struct ioport_operations pci_config_address_ops = {
	.io_in	= pci_config_address_in,
	.io_out	= pci_config_address_out,
};

static bool pci_device_exists(u8 bus_number, u8 device_number, u8 function_number)
{
	struct pci_device_header *dev;

	if (pci_config_address.bus_number != bus_number)
		return false;

	if (pci_config_address.function_number != function_number)
		return false;

	if (device_number >= PCI_MAX_DEVICES)
		return false;

	dev = pci_devices[device_number];

	return dev != NULL;
}

static bool pci_config_data_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	/*
	 * If someone accesses PCI configuration space offsets that are not
	 * aligned to 4 bytes, it uses ioports to signify that.
	 */
	pci_config_address.reg_offset = port - PCI_CONFIG_DATA;

	pci__config_wr(kvm, pci_config_address, data, size);

	return true;
}

static bool pci_config_data_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	/*
	 * If someone accesses PCI configuration space offsets that are not
	 * aligned to 4 bytes, it uses ioports to signify that.
	 */
	pci_config_address.reg_offset = port - PCI_CONFIG_DATA;

	pci__config_rd(kvm, pci_config_address, data, size);

	return true;
}

static struct ioport_operations pci_config_data_ops = {
	.io_in	= pci_config_data_in,
	.io_out	= pci_config_data_out,
};

void pci__config_wr(struct kvm *kvm, union pci_config_address addr, void *data, int size)
{
	u8 dev_num;

	dev_num	= addr.device_number;

	if (pci_device_exists(0, dev_num, 0)) {
		unsigned long offset;

		offset = addr.w & 0xff;
		if (offset < sizeof(struct pci_device_header)) {
			void *p = pci_devices[dev_num];
			u8 bar = (offset - PCI_BAR_OFFSET(0)) / (sizeof(u32));
			u32 sz = PCI_IO_SIZE;

			if (bar < 6 && pci_devices[dev_num]->bar_size[bar])
				sz = pci_devices[dev_num]->bar_size[bar];

			/*
			 * If the kernel masks the BAR it would expect to find the
			 * size of the BAR there next time it reads from it.
			 * When the kernel got the size it would write the address
			 * back.
			 */
			if (*(u32 *)(p + offset)) {
				/* See if kernel tries to mask one of the BARs */
				if ((offset >= PCI_BAR_OFFSET(0)) &&
				    (offset <= PCI_BAR_OFFSET(6)) &&
				    (ioport__read32(data)  == 0xFFFFFFFF))
					memcpy(p + offset, &sz, sizeof(sz));
				    else
					memcpy(p + offset, data, size);
			}
		}
	}
}

void pci__config_rd(struct kvm *kvm, union pci_config_address addr, void *data, int size)
{
	u8 dev_num;

	dev_num	= addr.device_number;

	if (pci_device_exists(0, dev_num, 0)) {
		unsigned long offset;

		offset = addr.w & 0xff;
		if (offset < sizeof(struct pci_device_header)) {
			void *p = pci_devices[dev_num];

			memcpy(data, p + offset, size);
		} else {
			memset(data, 0x00, size);
		}
	} else {
		memset(data, 0xff, size);
	}
}

int pci__register(struct pci_device_header *dev, u8 dev_num)
{
	if (dev_num >= PCI_MAX_DEVICES)
		return -ENOSPC;

	pci_devices[dev_num] = dev;

	return 0;
}

struct pci_device_header *pci__find_dev(u8 dev_num)
{
	if (dev_num >= PCI_MAX_DEVICES)
		return ERR_PTR(-EOVERFLOW);

	return pci_devices[dev_num];
}

int pci__init(struct kvm *kvm)
{
	int r;

	r = ioport__register(kvm, PCI_CONFIG_DATA + 0, &pci_config_data_ops, 4, NULL);
	if (r < 0)
		return r;

	r = ioport__register(kvm, PCI_CONFIG_ADDRESS + 0, &pci_config_address_ops, 4, NULL);
	if (r < 0) {
		ioport__unregister(kvm, PCI_CONFIG_DATA);
		return r;
	}

	return 0;
}
base_init(pci__init);

int pci__exit(struct kvm *kvm)
{
	ioport__unregister(kvm, PCI_CONFIG_DATA);
	ioport__unregister(kvm, PCI_CONFIG_ADDRESS);

	return 0;
}
base_exit(pci__exit);
