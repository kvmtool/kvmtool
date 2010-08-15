#include "kvm/blk-virtio.h"

#include "kvm/ioport.h"
#include "kvm/pci.h"

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

void blk_virtio__init(void)
{
	pci__register(&virtio_device, 1);

	ioport__register(IOPORT_VIRTIO, &virtio_io_ops, 1);
}
