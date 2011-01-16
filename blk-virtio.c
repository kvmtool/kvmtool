#include "kvm/blk-virtio.h"

#include "kvm/virtio_pci.h"
#include "kvm/disk-image.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <inttypes.h>
#include <assert.h>

#define VIRTIO_BLK_IRQ		14

#define NUM_VIRT_QUEUES		1

#define VIRTIO_BLK_QUEUE_SIZE	16

struct virt_queue {
	struct vring			vring;
	uint32_t			pfn;
	/* The last_avail_idx field is an index to ->ring of struct vring_avail.
	   It's where we assume the next request index is at.  */
	uint16_t			last_avail_idx;
};

struct device {
	struct virtio_blk_config	blk_config;
	uint32_t			host_features;
	uint32_t			guest_features;
	uint16_t			config_vector;
	uint8_t				status;

	/* virtio queue */
	uint16_t			queue_selector;

	struct virt_queue		virt_queues[NUM_VIRT_QUEUES];
};

#define DISK_CYLINDERS	1024
#define DISK_HEADS	64
#define DISK_SECTORS	32

static struct device device = {
	.blk_config		= (struct virtio_blk_config) {
		.capacity		= DISK_CYLINDERS * DISK_HEADS * DISK_SECTORS,
		/* VIRTIO_BLK_F_GEOMETRY */
		.geometry		= {
			.cylinders		= DISK_CYLINDERS,
			.heads			= DISK_HEADS,
			.sectors		= DISK_SECTORS,
		},
		/* VIRTIO_BLK_SIZE */
		.blk_size		= 4096,
	},
	/*
	 * Note we don't set VIRTIO_BLK_F_GEOMETRY here so the
	 * node kernel will compute disk geometry by own, the
	 * same applies to VIRTIO_BLK_F_BLK_SIZE
	 */
	.host_features		= 0,
};

static bool virtio_blk_config_in(void *data, unsigned long offset, int size, uint32_t count)
{
	uint8_t *config_space = (uint8_t *) &device.blk_config;

	if (size != 1 || count != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_PCI_CONFIG_NOMSI]);

	return true;
}

static bool blk_virtio_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO_BLK;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, device.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		return false;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, device.virt_queues[device.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_BLK_QUEUE_SIZE);
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
		ioport__write16(data, device.config_vector);
		break;
	default:
		return virtio_blk_config_in(data, offset, size, count);
	};

	return true;
}

static bool blk_virtio_read(struct kvm *self, struct virt_queue *queue)
{
	struct vring_used_elem *used_elem;
	struct virtio_blk_outhdr *req;
	struct vring_desc *desc;
	uint16_t desc_ndx;
	uint32_t block_len;
	uint8_t *status;
	void *block;

	desc_ndx		= queue->vring.avail->ring[queue->last_avail_idx++ % queue->vring.num];

	if (desc_ndx >= queue->vring.num) {
		warning("fatal I/O error");
		return false;
	}

	/* header */
	desc			= &queue->vring.desc[desc_ndx];
	assert(!(desc->flags & VRING_DESC_F_INDIRECT));

	req			= guest_flat_to_host(self, desc->addr);

	/* block */
	desc			= &queue->vring.desc[desc->next];
	assert(!(desc->flags & VRING_DESC_F_INDIRECT));

	block			= guest_flat_to_host(self, desc->addr);
	block_len		= desc->len;

	/* status */
	desc			= &queue->vring.desc[desc->next];
	assert(!(desc->flags & VRING_DESC_F_INDIRECT));

	status			= guest_flat_to_host(self, desc->addr);

	switch (req->type) {
	case VIRTIO_BLK_T_IN: {
		int err;

		err		= disk_image__read_sector(self->disk_image, req->sector, block, block_len);
		if (err)
			*status			= VIRTIO_BLK_S_IOERR;
		else
			*status			= VIRTIO_BLK_S_OK;
		break;
	}
	case VIRTIO_BLK_T_OUT: {
		int err;

		err		= disk_image__write_sector(self->disk_image, req->sector, block, block_len);
		if (err)
			*status			= VIRTIO_BLK_S_IOERR;
		else
			*status			= VIRTIO_BLK_S_OK;
		break;
	}
	default:
		warning("request type %d", req->type);
		*status			= VIRTIO_BLK_S_IOERR;
		break;
	}

	used_elem		= &queue->vring.used->ring[queue->vring.used->idx++ % queue->vring.num];

	used_elem->id		= desc_ndx;
	used_elem->len		= 3;

	return true;
}

static bool blk_virtio_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO_BLK;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		device.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		queue			= &device.virt_queues[device.queue_selector];

		queue->pfn		= ioport__read32(data);

		p			= guest_flat_to_host(self, queue->pfn << 12);

		vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, 4096);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		device.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		struct virt_queue *queue;
		uint16_t queue_index;

		queue_index		= ioport__read16(data);

		queue			= &device.virt_queues[queue_index];

		while (queue->vring.avail->idx != queue->last_avail_idx) {
			if (!blk_virtio_read(self, queue))
				return false;
		}
		kvm__irq_line(self, VIRTIO_BLK_IRQ, 1);

		break;
	}
	case VIRTIO_PCI_STATUS:
		device.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		device.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
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
	.bar[0]			= IOPORT_VIRTIO_BLK | PCI_BASE_ADDRESS_SPACE_IO,
	.irq_pin		= 1,
	.irq_line		= VIRTIO_BLK_IRQ,
};

void blk_virtio__init(struct kvm *self)
{
	if (!self->disk_image)
		return;

	device.blk_config.capacity = self->disk_image->size / SECTOR_SIZE;

	pci__register(&blk_virtio_pci_device, 1);

	ioport__register(IOPORT_VIRTIO_BLK, &blk_virtio_io_ops, 256);
}
