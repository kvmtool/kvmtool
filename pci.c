#include "kvm/pci.h"
#include "kvm/ioport.h"
#include "kvm/util.h"

#include <stdint.h>

static struct pci_config_address pci_config_address;

static void *pci_config_address_ptr(uint16_t port)
{
	unsigned long offset;
	void *base;

	offset		= port - PCI_CONFIG_ADDRESS;
	base		= &pci_config_address;

	return base + offset;
}

static bool pci_config_address_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	void *p = pci_config_address_ptr(port);

	memcpy(p, data, size);

	return true;
}

static bool pci_config_address_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	void *p = pci_config_address_ptr(port);

	memcpy(data, p, size);

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

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_BLK		0x0002

static struct pci_device_header virtio_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x010000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_BLK,
	.bar[0]			= IOPORT_VIRTIO | PCI_BASE_ADDRESS_SPACE_IO,
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
	unsigned long start;

	/*
	 * If someone accesses PCI configuration space offsets that are not
	 * aligned to 4 bytes, it uses ioports to signify that.
	 */
	start = port - PCI_CONFIG_DATA;

	if (pci_device_matches(0, 1, 0)) {
		unsigned long offset;

		offset = start + (pci_config_address.register_number << 2);
		if (offset < sizeof(struct pci_device_header)) {
			void *p = &virtio_device;
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

static bool virtio_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static bool virtio_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static struct ioport_operations virtio_io_ops = {
	.io_in		= virtio_in,
	.io_out		= virtio_out,
};

void pci__init(void)
{
	ioport__register(IOPORT_VIRTIO, &virtio_io_ops);

	ioport__register(PCI_CONFIG_DATA + 0, &pci_config_data_ops);
	ioport__register(PCI_CONFIG_DATA + 1, &pci_config_data_ops);
	ioport__register(PCI_CONFIG_DATA + 2, &pci_config_data_ops);
	ioport__register(PCI_CONFIG_DATA + 3, &pci_config_data_ops);

	ioport__register(PCI_CONFIG_ADDRESS + 0, &pci_config_address_ops);
	ioport__register(PCI_CONFIG_ADDRESS + 1, &pci_config_address_ops);
	ioport__register(PCI_CONFIG_ADDRESS + 2, &pci_config_address_ops);
	ioport__register(PCI_CONFIG_ADDRESS + 3, &pci_config_address_ops);
}
