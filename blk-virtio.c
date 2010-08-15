#include "kvm/blk-virtio.h"

#include "kvm/virtio_pci.h"
#include "kvm/ioport.h"
#include "kvm/pci.h"

#define VIRTIO_PCI_IOPORT_SIZE		24

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_BLK		0x0002

static struct pci_device_header blk_virtio_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x010000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_BLK,
	.bar[0]			= IOPORT_VIRTIO | PCI_BASE_ADDRESS_SPACE_IO,
};

static bool blk_virtio_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
	case VIRTIO_PCI_GUEST_FEATURES:
	case VIRTIO_PCI_QUEUE_PFN:
	case VIRTIO_PCI_QUEUE_NUM:
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
	case VIRTIO_PCI_STATUS:
	case VIRTIO_PCI_ISR:
	case VIRTIO_MSI_CONFIG_VECTOR:
		return true;
	};

	return false;
}

static bool blk_virtio_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
	case VIRTIO_PCI_QUEUE_PFN:
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
	case VIRTIO_PCI_STATUS:
	case VIRTIO_MSI_CONFIG_VECTOR:
	case VIRTIO_MSI_QUEUE_VECTOR:
		return true;
	};

	return false;
}

static struct ioport_operations blk_virtio_io_ops = {
	.io_in		= blk_virtio_in,
	.io_out		= blk_virtio_out,
};

void blk_virtio__init(void)
{
	pci__register(&blk_virtio_device, 1);

	ioport__register(IOPORT_VIRTIO, &blk_virtio_io_ops, VIRTIO_PCI_IOPORT_SIZE);
}
