#include "kvm/virtio-balloon.h"

#include "kvm/virtio-pci-dev.h"

#include "kvm/virtio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/guest_compat.h"
#include "kvm/kvm-ipc.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_balloon.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/eventfd.h>

#define NUM_VIRT_QUEUES		3
#define VIRTIO_BLN_QUEUE_SIZE	128
#define VIRTIO_BLN_INFLATE	0
#define VIRTIO_BLN_DEFLATE	1
#define VIRTIO_BLN_STATS	2

struct bln_dev {
	struct list_head	list;
	struct virtio_device	vdev;

	u32			features;

	/* virtio queue */
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct thread_pool__job	jobs[NUM_VIRT_QUEUES];

	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];
	struct virtio_balloon_stat *cur_stat;
	u32			cur_stat_head;
	u16			stat_count;
	int			stat_waitfd;

	struct virtio_balloon_config config;
};

static struct bln_dev bdev;
static int compat_id = -1;

static bool virtio_bln_do_io_request(struct kvm *kvm, struct bln_dev *bdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLN_QUEUE_SIZE];
	unsigned int len = 0;
	u16 out, in, head;
	u32 *ptrs, i;

	head	= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	ptrs	= iov[0].iov_base;
	len	= iov[0].iov_len / sizeof(u32);

	for (i = 0 ; i < len ; i++) {
		void *guest_ptr;

		guest_ptr = guest_flat_to_host(kvm, ptrs[i] << VIRTIO_BALLOON_PFN_SHIFT);
		if (queue == &bdev->vqs[VIRTIO_BLN_INFLATE]) {
			madvise(guest_ptr, 1 << VIRTIO_BALLOON_PFN_SHIFT, MADV_DONTNEED);
			bdev->config.actual++;
		} else if (queue == &bdev->vqs[VIRTIO_BLN_DEFLATE]) {
			bdev->config.actual--;
		}
	}

	virt_queue__set_used_elem(queue, head, len);

	return true;
}

static bool virtio_bln_do_stat_request(struct kvm *kvm, struct bln_dev *bdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLN_QUEUE_SIZE];
	u16 out, in, head;
	struct virtio_balloon_stat *stat;
	u64 wait_val = 1;

	head = virt_queue__get_iov(queue, iov, &out, &in, kvm);
	stat = iov[0].iov_base;

	/* Initial empty stat buffer */
	if (bdev->cur_stat == NULL) {
		bdev->cur_stat = stat;
		bdev->cur_stat_head = head;

		return true;
	}

	memcpy(bdev->stats, stat, iov[0].iov_len);

	bdev->stat_count = iov[0].iov_len / sizeof(struct virtio_balloon_stat);
	bdev->cur_stat = stat;
	bdev->cur_stat_head = head;

	if (write(bdev->stat_waitfd, &wait_val, sizeof(wait_val)) <= 0)
		return -EFAULT;

	return 1;
}

static void virtio_bln_do_io(struct kvm *kvm, void *param)
{
	struct virt_queue *vq = param;

	if (vq == &bdev.vqs[VIRTIO_BLN_STATS]) {
		virtio_bln_do_stat_request(kvm, &bdev, vq);
		bdev.vdev.ops->signal_vq(kvm, &bdev.vdev, VIRTIO_BLN_STATS);
		return;
	}

	while (virt_queue__available(vq)) {
		virtio_bln_do_io_request(kvm, &bdev, vq);
		bdev.vdev.ops->signal_vq(kvm, &bdev.vdev, vq - bdev.vqs);
	}
}

static int virtio_bln__collect_stats(struct kvm *kvm)
{
	u64 tmp;

	virt_queue__set_used_elem(&bdev.vqs[VIRTIO_BLN_STATS], bdev.cur_stat_head,
				  sizeof(struct virtio_balloon_stat));
	bdev.vdev.ops->signal_vq(kvm, &bdev.vdev, VIRTIO_BLN_STATS);

	if (read(bdev.stat_waitfd, &tmp, sizeof(tmp)) <= 0)
		return -EFAULT;

	return 0;
}

static void virtio_bln__print_stats(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	int r;

	if (WARN_ON(type != KVM_IPC_STAT || len))
		return;

	if (virtio_bln__collect_stats(kvm) < 0)
		return;

	r = write(fd, bdev.stats, sizeof(bdev.stats));
	if (r < 0)
		pr_warning("Failed sending memory stats");
}

static void handle_mem(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	int mem;

	if (WARN_ON(type != KVM_IPC_BALLOON || len != sizeof(int)))
		return;

	mem = *(int *)msg;
	if (mem > 0) {
		bdev.config.num_pages += 256 * mem;
	} else if (mem < 0) {
		if (bdev.config.num_pages < (u32)(256 * (-mem)))
			return;

		bdev.config.num_pages += 256 * mem;
	}

	/* Notify that the configuration space has changed */
	bdev.vdev.ops->signal_config(kvm, &bdev.vdev);
}

static u8 *get_config(struct kvm *kvm, void *dev)
{
	struct bln_dev *bdev = dev;

	return ((u8 *)(&bdev->config));
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return 1 << VIRTIO_BALLOON_F_STATS_VQ;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	struct bln_dev *bdev = dev;

	bdev->features = features;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 pfn)
{
	struct bln_dev *bdev = dev;
	struct virt_queue *queue;
	void *p;

	compat__remove_message(compat_id);

	queue		= &bdev->vqs[vq];
	queue->pfn	= pfn;
	p		= guest_pfn_to_host(kvm, queue->pfn);

	thread_pool__init_job(&bdev->jobs[vq], kvm, virtio_bln_do_io, queue);
	vring_init(&queue->vring, VIRTIO_BLN_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct bln_dev *bdev = dev;

	thread_pool__do_job(&bdev->jobs[vq]);

	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct bln_dev *bdev = dev;

	return bdev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_BLN_QUEUE_SIZE;
}

struct virtio_ops bln_dev_virtio_ops = (struct virtio_ops) {
	.get_config		= get_config,
	.get_host_features	= get_host_features,
	.set_guest_features	= set_guest_features,
	.init_vq		= init_vq,
	.notify_vq		= notify_vq,
	.get_pfn_vq		= get_pfn_vq,
	.get_size_vq		= get_size_vq,
};

int virtio_bln__init(struct kvm *kvm)
{
	if (!kvm->cfg.balloon)
		return 0;

	kvm_ipc__register_handler(KVM_IPC_BALLOON, handle_mem);
	kvm_ipc__register_handler(KVM_IPC_STAT, virtio_bln__print_stats);

	bdev.stat_waitfd	= eventfd(0, 0);
	memset(&bdev.config, 0, sizeof(struct virtio_balloon_config));

	virtio_init(kvm, &bdev, &bdev.vdev, &bln_dev_virtio_ops,
		    VIRTIO_PCI, PCI_DEVICE_ID_VIRTIO_BLN, VIRTIO_ID_BALLOON, PCI_CLASS_BLN);

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-balloon", "CONFIG_VIRTIO_BALLOON");

	return 0;
}
virtio_dev_init(virtio_bln__init);

int virtio_bln__exit(struct kvm *kvm)
{
	return 0;
}
virtio_dev_exit(virtio_bln__exit);
