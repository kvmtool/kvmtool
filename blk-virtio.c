#include "kvm/blk-virtio.h"

#include "kvm/virtio_blk.h"
#include "kvm/virtio_pci.h"
#include "kvm/ioport.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#define VIRTIO_BLK_IRQ		14

struct device {
	uint32_t		host_features;
	uint32_t		guest_features;
	uint8_t			status;
};

static struct device device = {
	.host_features		= (1UL << VIRTIO_BLK_F_GEOMETRY)
				| (1UL << VIRTIO_BLK_F_TOPOLOGY)
				| (1UL << VIRTIO_BLK_F_BLK_SIZE),
};

static bool blk_virtio_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO;

	/* XXX: Let virtio block device handle this */
	if (offset >= VIRTIO_PCI_CONFIG_NOMSI)
		return true;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, device.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		return false;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, 0x00);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, 0x10);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		return false;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, device.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, 0x1);
		kvm__irq_line(self, VIRTIO_BLK_IRQ, 0);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
	default:
		return false;
	};

	return true;
}

static bool blk_virtio_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO;

	/* XXX: Let virtio block device handle this */
	if (offset >= VIRTIO_PCI_CONFIG_NOMSI)
		return true;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		device.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		break;
	case VIRTIO_PCI_QUEUE_SEL:
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY:
		kvm__irq_line(self, VIRTIO_BLK_IRQ, 1);
		break;
	case VIRTIO_PCI_STATUS:
		device.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
	case VIRTIO_MSI_QUEUE_VECTOR:
	default:
		return false;
	};

	return true;
}

static struct ioport_operations blk_virtio_io_ops = {
	.io_in		= blk_virtio_in,
	.io_out		= blk_virtio_out,
};

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_BLK		0x0002

static struct pci_device_header blk_virtio_pci_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x010000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_BLK,
	.bar[0]			= IOPORT_VIRTIO | PCI_BASE_ADDRESS_SPACE_IO,
	.irq_pin		= 0,
	.irq_line		= VIRTIO_BLK_IRQ,
};

void blk_virtio__init(void)
{
	pci__register(&blk_virtio_pci_device, 1);

	ioport__register(IOPORT_VIRTIO, &blk_virtio_io_ops, 256);
}
