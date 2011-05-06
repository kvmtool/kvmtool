#include "kvm/virtio-blk.h"

#include "kvm/virtio-pci.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/irq.h"
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

#include <linux/types.h>
#include <pthread.h>

#define VIRTIO_BLK_MAX_DEV		4
#define NUM_VIRT_QUEUES			1

#define VIRTIO_BLK_QUEUE_SIZE		128

struct blk_dev_job {
	struct virt_queue		*vq;
	struct blk_dev			*bdev;
	void				*job_id;
};

struct blk_dev {
	pthread_mutex_t			mutex;

	struct virtio_blk_config	blk_config;
	struct disk_image		*disk;
	u32				host_features;
	u32				guest_features;
	u16				config_vector;
	u8				status;
	u8				idx;

	/* virtio queue */
	u16				queue_selector;

	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct blk_dev_job		jobs[NUM_VIRT_QUEUES];
	struct pci_device_header	pci_device;
};

static struct blk_dev *bdevs[VIRTIO_BLK_MAX_DEV];

static bool virtio_blk_dev_in(struct blk_dev *bdev, void *data, unsigned long offset, int size, u32 count)
{
	u8 *config_space = (u8 *) &bdev->blk_config;

	if (size != 1 || count != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_PCI_CONFIG_NOMSI]);

	return true;
}

/* Translate port into device id + offset in that device addr space */
static void virtio_blk_port2dev(u16 port, u16 base, u16 size, u16 *dev_idx, u16 *offset)
{
	*dev_idx	= (port - base) / size;
	*offset		= port - (base + *dev_idx * size);
}

static bool virtio_blk_pci_io_in(struct kvm *self, u16 port, void *data, int size, u32 count)
{
	struct blk_dev *bdev;
	u16 offset, dev_idx;
	bool ret = true;

	virtio_blk_port2dev(port, IOPORT_VIRTIO_BLK, IOPORT_VIRTIO_BLK_SIZE, &dev_idx, &offset);

	bdev = bdevs[dev_idx];

	mutex_lock(&bdev->mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, bdev->host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret		= false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, bdev->vqs[bdev->queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_BLK_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret		= false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, bdev->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, 0x1);
		kvm__irq_line(self, bdev->pci_device.irq_line, 0);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, bdev->config_vector);
		break;
	default:
		ret = virtio_blk_dev_in(bdev, data, offset, size, count);
		break;
	};

	mutex_unlock(&bdev->mutex);

	return ret;
}

static bool virtio_blk_do_io_request(struct kvm *self,
					struct blk_dev *bdev,
					struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLK_QUEUE_SIZE];
	struct virtio_blk_outhdr *req;
	ssize_t block_cnt = -1;
	u16 out, in, head;
	u8 *status;

	head			= virt_queue__get_iov(queue, iov, &out, &in, self);

	/* head */
	req			= iov[0].iov_base;

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
		block_cnt	= disk_image__read_sector_iov(bdev->disk, req->sector, iov + 1, in + out - 2);
		break;
	case VIRTIO_BLK_T_OUT:
		block_cnt	= disk_image__write_sector_iov(bdev->disk, req->sector, iov + 1, in + out - 2);
		break;
	default:
		warning("request type %d", req->type);
		block_cnt	= -1;
		break;
	}

	/* status */
	status			= iov[out + in - 1].iov_base;
	*status			= (block_cnt < 0) ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

	virt_queue__set_used_elem(queue, head, block_cnt);

	return true;
}

static void virtio_blk_do_io(struct kvm *kvm, void *param)
{
	struct blk_dev_job *job	= param;
	struct virt_queue *vq;
	struct blk_dev *bdev;

	vq			= job->vq;
	bdev			= job->bdev;

	while (virt_queue__available(vq))
		virtio_blk_do_io_request(kvm, bdev, vq);

	kvm__irq_line(kvm, bdev->pci_device.irq_line, 1);
}

static bool virtio_blk_pci_io_out(struct kvm *self, u16 port, void *data, int size, u32 count)
{
	struct blk_dev *bdev;
	u16 offset, dev_idx;
	bool ret = true;

	virtio_blk_port2dev(port, IOPORT_VIRTIO_BLK, IOPORT_VIRTIO_BLK_SIZE, &dev_idx, &offset);

	bdev = bdevs[dev_idx];

	mutex_lock(&bdev->mutex);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		bdev->guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		struct blk_dev_job *job;
		void *p;

		job = &bdev->jobs[bdev->queue_selector];

		queue			= &bdev->vqs[bdev->queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_flat_to_host(self, queue->pfn << 12);

		vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, 4096);

		*job			= (struct blk_dev_job) {
			.vq			= queue,
			.bdev			= bdev,
		};

		job->job_id = thread_pool__add_job(self, virtio_blk_do_io, job);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		bdev->queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;

		queue_index		= ioport__read16(data);
		thread_pool__do_job(bdev->jobs[queue_index].job_id);

		break;
	}
	case VIRTIO_PCI_STATUS:
		bdev->status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		bdev->config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret			= false;
		break;
	};

	mutex_unlock(&bdev->mutex);

	return ret;
}

static struct ioport_operations virtio_blk_io_ops = {
	.io_in		= virtio_blk_pci_io_in,
	.io_out		= virtio_blk_pci_io_out,
};

static int virtio_blk_find_empty_dev(void)
{
	int i;

	for (i = 0; i < VIRTIO_BLK_MAX_DEV; i++) {
		if (bdevs[i] == NULL)
			return i;
	}

	return -1;
}

void virtio_blk__init(struct kvm *self, struct disk_image *disk)
{
	u16 blk_dev_base_addr;
	u8 dev, pin, line;
	struct blk_dev *bdev;
	int new_dev_idx;

	if (!disk)
		return;

	new_dev_idx		= virtio_blk_find_empty_dev();
	if (new_dev_idx < 0)
		die("Could not find an empty block device slot");

	bdevs[new_dev_idx]	= calloc(1, sizeof(struct blk_dev));
	if (bdevs[new_dev_idx] == NULL)
		die("Failed allocating bdev");

	bdev			= bdevs[new_dev_idx];

	blk_dev_base_addr	= IOPORT_VIRTIO_BLK + new_dev_idx * IOPORT_VIRTIO_BLK_SIZE;

	*bdev			= (struct blk_dev) {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,
		.disk			= disk,
		.idx			= new_dev_idx,
		.blk_config		= (struct virtio_blk_config) {
			.capacity		= disk->size / SECTOR_SIZE,
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
		},
	};

	if (irq__register_device(PCI_DEVICE_ID_VIRTIO_BLK, &dev, &pin, &line) < 0)
		return;

	bdev->pci_device.irq_pin	= pin;
	bdev->pci_device.irq_line	= line;

	pci__register(&bdev->pci_device, dev);

	ioport__register(blk_dev_base_addr, &virtio_blk_io_ops, IOPORT_VIRTIO_BLK_SIZE);
}
