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

#include <linux/byteorder.h>
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
	u32 actual;

	head	= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	ptrs	= iov[0].iov_base;
	len	= iov[0].iov_len / sizeof(u32);

	actual = le32_to_cpu(bdev->config.actual);
	for (i = 0 ; i < len ; i++) {
		void *guest_ptr;

		guest_ptr = guest_flat_to_host(kvm, (u64)ptrs[i] << VIRTIO_BALLOON_PFN_SHIFT);
		if (queue == &bdev->vqs[VIRTIO_BLN_INFLATE]) {
			madvise(guest_ptr, 1 << VIRTIO_BALLOON_PFN_SHIFT, MADV_DONTNEED);
			actual++;
		} else if (queue == &bdev->vqs[VIRTIO_BLN_DEFLATE]) {
			actual--;
		}
	}
	bdev->config.actual = cpu_to_le32(actual);

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
	struct virt_queue *vq = &bdev.vqs[VIRTIO_BLN_STATS];
	u64 tmp;

	/* Exit if the queue is not set up. */
	if (!vq->enabled)
		return -ENODEV;

	virt_queue__set_used_elem(vq, bdev.cur_stat_head,
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
	u32 num_pages;

	if (WARN_ON(type != KVM_IPC_BALLOON || len != sizeof(int)))
		return;

	mem = *(int *)msg;
	num_pages = le32_to_cpu(bdev.config.num_pages);

	if (mem > 0) {
		num_pages += 256 * mem;
	} else if (mem < 0) {
		if (num_pages < (u32)(256 * (-mem)))
			return;

		num_pages += 256 * mem;
	}

	bdev.config.num_pages = cpu_to_le32(num_pages);

	/* Notify that the configuration space has changed */
	bdev.vdev.ops->signal_config(kvm, &bdev.vdev);
}

static u8 *get_config(struct kvm *kvm, void *dev)
{
	struct bln_dev *bdev = dev;

	return ((u8 *)(&bdev->config));
}

static size_t get_config_size(struct kvm *kvm, void *dev)
{
	struct bln_dev *bdev = dev;

	return sizeof(bdev->config);
}

static u64 get_host_features(struct kvm *kvm, void *dev)
{
	return 1 << VIRTIO_BALLOON_F_STATS_VQ;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct bln_dev *bdev = dev;
	struct virt_queue *queue;

	compat__remove_message(compat_id);

	queue		= &bdev->vqs[vq];

	virtio_init_device_vq(kvm, &bdev->vdev, queue, VIRTIO_BLN_QUEUE_SIZE);

	thread_pool__init_job(&bdev->jobs[vq], kvm, virtio_bln_do_io, queue);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct bln_dev *bdev = dev;

	thread_pool__do_job(&bdev->jobs[vq]);

	return 0;
}

static struct virt_queue *get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct bln_dev *bdev = dev;

	return &bdev->vqs[vq];
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_BLN_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	/* FIXME: dynamic */
	return size;
}

static unsigned int get_vq_count(struct kvm *kvm, void *dev)
{
	return NUM_VIRT_QUEUES;
}

struct virtio_ops bln_dev_virtio_ops = {
	.get_config		= get_config,
	.get_config_size	= get_config_size,
	.get_host_features	= get_host_features,
	.init_vq		= init_vq,
	.notify_vq		= notify_vq,
	.get_vq			= get_vq,
	.get_size_vq		= get_size_vq,
	.set_size_vq            = set_size_vq,
	.get_vq_count		= get_vq_count,
};

int virtio_bln__init(struct kvm *kvm)
{
	int r;

	if (!kvm->cfg.balloon)
		return 0;

	kvm_ipc__register_handler(KVM_IPC_BALLOON, handle_mem);
	kvm_ipc__register_handler(KVM_IPC_STAT, virtio_bln__print_stats);

	bdev.stat_waitfd	= eventfd(0, 0);
	memset(&bdev.config, 0, sizeof(struct virtio_balloon_config));

	r = virtio_init(kvm, &bdev, &bdev.vdev, &bln_dev_virtio_ops,
			VIRTIO_DEFAULT_TRANS(kvm), PCI_DEVICE_ID_VIRTIO_BLN,
			VIRTIO_ID_BALLOON, PCI_CLASS_BLN);
	if (r < 0)
		return r;

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
