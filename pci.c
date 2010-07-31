#include "kvm/pci.h"
#include "kvm/ioport.h"
#include "kvm/util.h"

#include <stdint.h>

static struct pci_config_address pci_config_address;

static bool pci_config_address_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	struct pci_config_address *addr = data;

	pci_config_address	= *addr;

	return true;
}

static bool pci_config_address_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	struct pci_config_address *addr = data;

	*addr		=  pci_config_address;

	return true;
}

static struct ioport_operations pci_config_address_ops = {
	.io_in		= pci_config_address_in,
	.io_out		= pci_config_address_out,
};

static bool pci_config_data_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

#define PCI_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK	0x1001

static struct pci_device_header virtio_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
};

static bool pci_device_matches(uint8_t bus_number, uint8_t device_number, uint8_t function_number)
{
	if (pci_config_address.bus_number != bus_number)
		return false;

	if (pci_config_address.device_number != device_number)
		return false;

	return pci_config_address.function_number == function_number;
}

static bool pci_config_data_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	if (pci_device_matches(0, 1, 0)) {
		unsigned long offset;

		offset		= pci_config_address.register_number << 2;
		if (offset < sizeof(struct pci_device_header)) {
			void *p = &virtio_device;

			memcpy(data, p + (pci_config_address.register_number << 2), size);
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

void pci__init(void)
{
	ioport__register(PCI_CONFIG_DATA,    &pci_config_data_ops);
	ioport__register(PCI_CONFIG_ADDRESS, &pci_config_address_ops);
}
