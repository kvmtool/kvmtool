#include "kvm/virtio-blk.h"

#include "kvm/virtio-pci-dev.h"
#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/ioeventfd.h"
#include "kvm/guest_compat.h"
#include "kvm/virtio-pci.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/types.h>
#include <pthread.h>

#define VIRTIO_BLK_MAX_DEV		4
#define NUM_VIRT_QUEUES			1

#define VIRTIO_BLK_QUEUE_SIZE		128
/*
 * the header and status consume too entries
 */
#define DISK_SEG_MAX			(VIRTIO_BLK_QUEUE_SIZE - 2)

struct blk_dev_job {
	struct virt_queue		*vq;
	struct blk_dev			*bdev;
	struct iovec			iov[VIRTIO_BLK_QUEUE_SIZE];
	u16				out, in, head;
	struct thread_pool__job		job_id;
};

struct blk_dev {
	pthread_mutex_t			mutex;
	struct list_head		list;

	struct virtio_pci		vpci;
	struct virtio_blk_config	blk_config;
	struct disk_image		*disk;
	int				compat_id;
	u32				features;

	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct blk_dev_job		jobs[VIRTIO_BLK_QUEUE_SIZE];
	u16				job_idx;
};

static LIST_HEAD(bdevs);

static void virtio_blk_do_io_request(struct kvm *kvm, void *param)
{
	struct virtio_blk_outhdr *req;
	u8 *status;
	ssize_t block_cnt;
	struct blk_dev_job *job;
	struct blk_dev *bdev;
	struct virt_queue *queue;
	struct iovec *iov;
	u16 out, in, head;

	block_cnt	= -1;
	job		= param;
	bdev		= job->bdev;
	queue		= job->vq;
	iov		= job->iov;
	out		= job->out;
	in		= job->in;
	head		= job->head;
	req		= iov[0].iov_base;

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
		block_cnt	= disk_image__read(bdev->disk, req->sector, iov + 1, in + out - 2);
		break;
	case VIRTIO_BLK_T_OUT:
		block_cnt	= disk_image__write(bdev->disk, req->sector, iov + 1, in + out - 2);
		break;
	case VIRTIO_BLK_T_FLUSH:
		block_cnt       = disk_image__flush(bdev->disk);
		break;
	case VIRTIO_BLK_T_GET_ID:
		block_cnt	= VIRTIO_BLK_ID_BYTES;
		disk_image__get_serial(bdev->disk, (iov + 1)->iov_base, &block_cnt);
		break;
	default:
		pr_warning("request type %d", req->type);
		block_cnt	= -1;
		break;
	}

	/* status */
	status			= iov[out + in - 1].iov_base;
	*status			= (block_cnt < 0) ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

	mutex_lock(&bdev->mutex);
	virt_queue__set_used_elem(queue, head, block_cnt);
	mutex_unlock(&bdev->mutex);

	virtio_pci__signal_vq(kvm, &bdev->vpci, queue - bdev->vqs);
}

static void virtio_blk_do_io(struct kvm *kvm, struct virt_queue *vq, struct blk_dev *bdev)
{
	while (virt_queue__available(vq)) {
		struct blk_dev_job *job = &bdev->jobs[bdev->job_idx++ % VIRTIO_BLK_QUEUE_SIZE];

		*job			= (struct blk_dev_job) {
			.vq			= vq,
			.bdev			= bdev,
		};
		job->head = virt_queue__get_iov(vq, job->iov, &job->out, &job->in, kvm);

		thread_pool__init_job(&job->job_id, kvm, virtio_blk_do_io_request, job);
		thread_pool__do_job(&job->job_id);
	}
}

static void ioevent_callback(struct kvm *kvm, void *param)
{
	struct blk_dev *bdev = param;

	virtio_blk_do_io(kvm, &bdev->vqs[0], bdev);
}

static void set_config(struct kvm *kvm, void *dev, u8 data, u32 offset)
{
	struct blk_dev *bdev = dev;

	((u8 *)(&bdev->blk_config))[offset] = data;
}

static u8 get_config(struct kvm *kvm, void *dev, u32 offset)
{
	struct blk_dev *bdev = dev;

	return ((u8 *)(&bdev->blk_config))[offset];
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return 1UL << VIRTIO_BLK_F_SEG_MAX | 1UL << VIRTIO_BLK_F_FLUSH;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	struct blk_dev *bdev = dev;

	bdev->features = features;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 pfn)
{
	struct blk_dev *bdev = dev;
	struct virt_queue *queue;
	void *p;
	struct ioevent ioevent;

	compat__remove_message(bdev->compat_id);

	queue			= &bdev->vqs[vq];
	queue->pfn		= pfn;
	p			= guest_pfn_to_host(kvm, queue->pfn);

	vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

	ioevent = (struct ioevent) {
		.io_addr	= bdev->vpci.base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
		.io_len		= sizeof(u16),
		.fn		= ioevent_callback,
		.fn_ptr		= bdev,
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

	ioeventfd__add_event(&ioevent);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;

	virtio_blk_do_io(kvm, &bdev->vqs[vq], bdev);

	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;

	return bdev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_BLK_QUEUE_SIZE;
}

void virtio_blk__init(struct kvm *kvm, struct disk_image *disk)
{
	struct blk_dev *bdev;

	if (!disk)
		return;

	bdev = calloc(1, sizeof(struct blk_dev));
	if (bdev == NULL)
		die("Failed allocating bdev");

	*bdev = (struct blk_dev) {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,
		.disk			= disk,
		.blk_config		= (struct virtio_blk_config) {
			.capacity	= disk->size / SECTOR_SIZE,
			.seg_max	= DISK_SEG_MAX,
		},
	};

	virtio_pci__init(kvm, &bdev->vpci, bdev, PCI_DEVICE_ID_VIRTIO_BLK, VIRTIO_ID_BLOCK);
	bdev->vpci.ops = (struct virtio_pci_ops) {
		.set_config		= set_config,
		.get_config		= get_config,
		.get_host_features	= get_host_features,
		.set_guest_features	= set_guest_features,
		.init_vq		= init_vq,
		.notify_vq		= notify_vq,
		.get_pfn_vq		= get_pfn_vq,
		.get_size_vq		= get_size_vq,
	};

	list_add_tail(&bdev->list, &bdevs);

	bdev->compat_id = compat__add_message("virtio-blk device was not detected",
						"While you have requested a virtio-blk device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_VIRTIO_BLK.");
}

void virtio_blk__init_all(struct kvm *kvm)
{
	int i;

	for (i = 0; i < kvm->nr_disks; i++)
		virtio_blk__init(kvm, kvm->disks[i]);
}

void virtio_blk__delete_all(struct kvm *kvm)
{
	while (!list_empty(&bdevs)) {
		struct blk_dev *bdev;

		bdev = list_first_entry(&bdevs, struct blk_dev, list);
		ioeventfd__del_event(bdev->vpci.base_addr + VIRTIO_PCI_QUEUE_NOTIFY, 0);
		list_del(&bdev->list);
		free(bdev);
	}
}
