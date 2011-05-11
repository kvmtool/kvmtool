#include "kvm/pci.h"
#include "kvm/ioport.h"
#include "kvm/util.h"

#include <assert.h>

#define PCI_MAX_DEVICES			256

static struct pci_device_header		*pci_devices[PCI_MAX_DEVICES];

static struct pci_config_address	pci_config_address;

static void *pci_config_address_ptr(u16 port)
{
	unsigned long offset;
	void *base;

	offset		= port - PCI_CONFIG_ADDRESS;
	base		= &pci_config_address;

	return base + offset;
}

static bool pci_config_address_out(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	void *p = pci_config_address_ptr(port);

	memcpy(p, data, size);

	return true;
}

static bool pci_config_address_in(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	void *p = pci_config_address_ptr(port);

	memcpy(data, p, size);

	return true;
}

static struct ioport_operations pci_config_address_ops = {
	.io_in		= pci_config_address_in,
	.io_out		= pci_config_address_out,
};

static bool pci_config_data_out(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	return true;
}

static bool pci_device_exists(u8 bus_number, u8 device_number, u8 function_number)
{
	struct pci_device_header *dev;

	if (pci_config_address.bus_number != bus_number)
		return false;

	if (pci_config_address.function_number != function_number)
		return false;

	if (device_number >= PCI_MAX_DEVICES)
		return false;

	dev		= pci_devices[device_number];

	return dev != NULL;
}

static bool pci_config_data_in(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	unsigned long start;
	u8 dev_num;

	/*
	 * If someone accesses PCI configuration space offsets that are not
	 * aligned to 4 bytes, it uses ioports to signify that.
	 */
	start = port - PCI_CONFIG_DATA;

	dev_num		= pci_config_address.device_number;

	if (pci_device_exists(0, dev_num, 0)) {
		unsigned long offset;

		offset = start + (pci_config_address.register_number << 2);
		if (offset < sizeof(struct pci_device_header)) {
			void *p = pci_devices[dev_num];

			memcpy(data, p + offset, size);
		} else
			memset(data, 0x00, size);
	} else
		memset(data, 0xff, size);

	return true;
}

static struct ioport_operations pci_config_data_ops = {
	.io_in		= pci_config_data_in,
	.io_out		= pci_config_data_out,
};

void pci__register(struct pci_device_header *dev, u8 dev_num)
{
	assert(dev_num < PCI_MAX_DEVICES);

	pci_devices[dev_num]	= dev;
}

void pci__init(void)
{
	ioport__register(PCI_CONFIG_DATA + 0, &pci_config_data_ops, 4);
	ioport__register(PCI_CONFIG_ADDRESS + 0, &pci_config_address_ops, 4);
}
