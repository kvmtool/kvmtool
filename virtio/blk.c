#include "kvm/virtio-blk.h"

#include "kvm/virtio-pci.h"

#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>

#include <inttypes.h>
#include <pthread.h>

#define VIRTIO_BLK_IRQ		9
#define VIRTIO_BLK_PIN		1
#define VIRTIO_BLK_MAX_DEV	4
#define NUM_VIRT_QUEUES		1

#define VIRTIO_BLK_QUEUE_SIZE	128

struct blk_device_job {
	struct virt_queue		*vq;
	struct blk_device		*blk_device;
	void				*job_id;
};

struct blk_device {
	pthread_mutex_t			mutex;

	struct virtio_blk_config	blk_config;
	struct disk_image		*disk;
	uint32_t			host_features;
	uint32_t			guest_features;
	uint16_t			config_vector;
	uint8_t				status;
	u8				idx;

	/* virtio queue */
	uint16_t			queue_selector;

	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct blk_device_job		jobs[NUM_VIRT_QUEUES];
	struct pci_device_header	pci_device;
};

static struct blk_device *blk_devices[VIRTIO_BLK_MAX_DEV];

static bool virtio_blk_pci_io_device_specific_in(struct blk_device *blk_device,
						void *data,
						unsigned long offset,
						int size,
						uint32_t count)
{
	uint8_t *config_space = (uint8_t *) &blk_device->blk_config;

	if (size != 1 || count != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_PCI_CONFIG_NOMSI]);

	return true;
}

/* Translate port into device id + offset in that device addr space */
static void virtio_blk_port2dev(u16 port,
				u16 base,
				u16 size,
				u16 *dev_idx,
				u16 *offset)
{
	*dev_idx	= (port - base) / size;
	*offset		= port - (base + *dev_idx * size);
}
static bool virtio_blk_pci_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	u16 offset, dev_idx;
	bool ret = true;
	struct blk_device *blk_device;

	virtio_blk_port2dev(port, IOPORT_VIRTIO_BLK, IOPORT_VIRTIO_BLK_SIZE,
				&dev_idx, &offset);

	blk_device = blk_devices[dev_idx];

	mutex_lock(&blk_device->mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, blk_device->host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret		= false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, blk_device->vqs[blk_device->queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_BLK_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret		= false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, blk_device->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, 0x1);
		kvm__irq_line(self, VIRTIO_BLK_IRQ + blk_device->idx, 0);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, blk_device->config_vector);
		break;
	default:
		ret = virtio_blk_pci_io_device_specific_in(blk_device, data, offset, size, count);
	};

	mutex_unlock(&blk_device->mutex);

	return ret;
}

static bool virtio_blk_do_io_request(struct kvm *self,
					struct blk_device *blk_device,
					struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLK_QUEUE_SIZE];
	struct virtio_blk_outhdr *req;
	ssize_t block_cnt = -1;
	uint16_t out, in, head;
	uint8_t *status;

	head		= virt_queue__get_iov(queue, iov, &out, &in, self);

	/* head */
	req		= iov[0].iov_base;

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
		block_cnt = disk_image__read_sector_iov(blk_device->disk, req->sector, iov + 1, in + out - 2);

		break;
	case VIRTIO_BLK_T_OUT:
		block_cnt = disk_image__write_sector_iov(blk_device->disk, req->sector, iov + 1, in + out - 2);

		break;

	default:
		warning("request type %d", req->type);
		block_cnt = -1;
	}

	/* status */
	status			= iov[out + in - 1].iov_base;
	*status			= (block_cnt < 0) ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

	virt_queue__set_used_elem(queue, head, block_cnt);

	return true;
}

static void virtio_blk_do_io(struct kvm *kvm, void *param)
{
	struct blk_device_job *job = param;
	struct virt_queue *vq = job->vq;
	struct blk_device *blk_device = job->blk_device;

	while (virt_queue__available(vq))
		virtio_blk_do_io_request(kvm, blk_device, vq);

	kvm__irq_line(kvm, VIRTIO_BLK_IRQ + blk_device->idx, 1);
}

static bool virtio_blk_pci_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	u16 offset, dev_idx;
	bool ret = true;
	struct blk_device *blk_device;

	virtio_blk_port2dev(port, IOPORT_VIRTIO_BLK, IOPORT_VIRTIO_BLK_SIZE,
						&dev_idx, &offset);

	blk_device = blk_devices[dev_idx];

	mutex_lock(&blk_device->mutex);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		blk_device->guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		struct blk_device_job *job;
		void *p;

		job = &blk_device->jobs[blk_device->queue_selector];

		queue			= &blk_device->vqs[blk_device->queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_flat_to_host(self, queue->pfn << 12);

		vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, 4096);

		*job = (struct blk_device_job) {
			.vq		= queue,
			.blk_device	= blk_device,
		};

		job->job_id = thread_pool__add_job(self, virtio_blk_do_io, job);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		blk_device->queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		uint16_t queue_index;
		queue_index		= ioport__read16(data);
		thread_pool__do_job(blk_device->jobs[queue_index].job_id);
		break;
	}
	case VIRTIO_PCI_STATUS:
		blk_device->status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		blk_device->config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret		= false;
	};

	mutex_unlock(&blk_device->mutex);

	return ret;
}

static struct ioport_operations virtio_blk_io_ops = {
	.io_in		= virtio_blk_pci_io_in,
	.io_out		= virtio_blk_pci_io_out,
};

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_BLK		0x0002
#define PCI_VIRTIO_BLK_DEVNUM 10

static int virtio_blk_find_empty_dev(void)
{
	int i;

	for (i = 0; i < VIRTIO_BLK_MAX_DEV; i++) {
		if (blk_devices[i] == NULL)
			return i;
	}

	return -1;
}

void virtio_blk__init(struct kvm *self, struct disk_image *disk)
{
	int new_dev_idx;
	u16 blk_dev_base_addr;
	struct blk_device *blk_device;

	if (!disk)
		return;

	new_dev_idx = virtio_blk_find_empty_dev();
	if (new_dev_idx < 0)
		die("Could not find an empty block device slot");

	blk_devices[new_dev_idx] = calloc(1, sizeof(struct blk_device));
	if (blk_devices[new_dev_idx] == NULL)
		die("Failed allocating blk_device");

	blk_device = blk_devices[new_dev_idx];
	blk_dev_base_addr = IOPORT_VIRTIO_BLK + new_dev_idx * IOPORT_VIRTIO_BLK_SIZE;

	*blk_device = (struct blk_device) {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,
		.disk			= disk,
		.idx			= new_dev_idx,
		.blk_config		= (struct virtio_blk_config) {
			.capacity	= disk->size / SECTOR_SIZE,
		},
		.pci_device = (struct pci_device_header) {
			.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
			.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
			.header_type		= PCI_HEADER_TYPE_NORMAL,
			.revision_id		= 0,
			.class			= 0x010000,
			.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
			.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_BLK,
			.bar[0]			= blk_dev_base_addr | PCI_BASE_ADDRESS_SPACE_IO,
			.irq_pin		= VIRTIO_BLK_PIN,
			.irq_line		= VIRTIO_BLK_IRQ + new_dev_idx,
		},
	};

	pci__register(&blk_device->pci_device, PCI_VIRTIO_BLK_DEVNUM + new_dev_idx);

	ioport__register(blk_dev_base_addr, &virtio_blk_io_ops, IOPORT_VIRTIO_BLK_SIZE);
}
