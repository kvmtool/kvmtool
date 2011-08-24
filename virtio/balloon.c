#include "kvm/virtio-balloon.h"

#include "kvm/virtio-pci-dev.h"

#include "kvm/virtio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/guest_compat.h"
#include "kvm/virtio-pci.h"

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
	struct virtio_pci	vpci;

	u32			features;

	/* virtio queue */
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct thread_pool__job	jobs[NUM_VIRT_QUEUES];

	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];
	struct virtio_balloon_stat *cur_stat;
	u32			cur_stat_head;
	u16			stat_count;
	int			stat_waitfd;

	int			compat_id;
	struct virtio_balloon_config config;
};

static struct bln_dev bdev;
extern struct kvm *kvm;

static bool virtio_bln_do_io_request(struct kvm *kvm, struct bln_dev *bdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLN_QUEUE_SIZE];
	unsigned int len = 0;
	u16 out, in, head;
	u32 *ptrs, i;

	head		= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	ptrs		= iov[0].iov_base;
	len		= iov[0].iov_len / sizeof(u32);

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
		virtio_pci__signal_vq(kvm, &bdev.vpci, VIRTIO_BLN_STATS);
		return;
	}

	while (virt_queue__available(vq)) {
		virtio_bln_do_io_request(kvm, &bdev, vq);
		virtio_pci__signal_vq(kvm, &bdev.vpci, vq - bdev.vqs);
	}
}

static int virtio_bln__collect_stats(void)
{
	u64 tmp;

	virt_queue__set_used_elem(&bdev.vqs[VIRTIO_BLN_STATS], bdev.cur_stat_head,
				  sizeof(struct virtio_balloon_stat));
	virtio_pci__signal_vq(kvm, &bdev.vpci, VIRTIO_BLN_STATS);

	if (read(bdev.stat_waitfd, &tmp, sizeof(tmp)) <= 0)
		return -EFAULT;

	return 0;
}

static int virtio_bln__print_stats(void)
{
	u16 i;

	if (virtio_bln__collect_stats() < 0)
		return -EFAULT;

	printf("\n\n\t*** Guest memory statistics ***\n\n");
	for (i = 0; i < bdev.stat_count; i++) {
		switch (bdev.stats[i].tag) {
		case VIRTIO_BALLOON_S_SWAP_IN:
			printf("The amount of memory that has been swapped in (in bytes):");
			break;
		case VIRTIO_BALLOON_S_SWAP_OUT:
			printf("The amount of memory that has been swapped out to disk (in bytes):");
			break;
		case VIRTIO_BALLOON_S_MAJFLT:
			printf("The number of major page faults that have occurred:");
			break;
		case VIRTIO_BALLOON_S_MINFLT:
			printf("The number of minor page faults that have occurred:");
			break;
		case VIRTIO_BALLOON_S_MEMFREE:
			printf("The amount of memory not being used for any purpose (in bytes):");
			break;
		case VIRTIO_BALLOON_S_MEMTOT:
			printf("The total amount of memory available (in bytes):");
			break;
		}
		printf("%llu\n", bdev.stats[i].val);
	}
	printf("\n");

	return 0;
}

static void handle_sigmem(int sig)
{
	if (sig == SIGKVMADDMEM) {
		bdev.config.num_pages += 256;
	} else if (sig == SIGKVMDELMEM) {
		if (bdev.config.num_pages < 256)
			return;

		bdev.config.num_pages -= 256;
	} else if (sig == SIGKVMMEMSTAT) {
		virtio_bln__print_stats();

		return;
	}

	/* Notify that the configuration space has changed */
	virtio_pci__signal_config(kvm, &bdev.vpci);
}

static void set_config(struct kvm *kvm, void *dev, u8 data, u32 offset)
{
	struct bln_dev *bdev = dev;

	((u8 *)(&bdev->config))[offset] = data;
}

static u8 get_config(struct kvm *kvm, void *dev, u32 offset)
{
	struct bln_dev *bdev = dev;

	return ((u8 *)(&bdev->config))[offset];
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

	compat__remove_message(bdev->compat_id);

	queue			= &bdev->vqs[vq];
	queue->pfn		= pfn;
	p			= guest_pfn_to_host(kvm, queue->pfn);

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

void virtio_bln__init(struct kvm *kvm)
{
	signal(SIGKVMADDMEM, handle_sigmem);
	signal(SIGKVMDELMEM, handle_sigmem);
	signal(SIGKVMMEMSTAT, handle_sigmem);

	bdev.stat_waitfd	= eventfd(0, 0);
	memset(&bdev.config, 0, sizeof(struct virtio_balloon_config));

	virtio_pci__init(kvm, &bdev.vpci, &bdev, PCI_DEVICE_ID_VIRTIO_BLN, VIRTIO_ID_BALLOON);
	bdev.vpci.ops = (struct virtio_pci_ops) {
		.set_config		= set_config,
		.get_config		= get_config,
		.get_host_features	= get_host_features,
		.set_guest_features	= set_guest_features,
		.init_vq		= init_vq,
		.notify_vq		= notify_vq,
		.get_pfn_vq		= get_pfn_vq,
		.get_size_vq		= get_size_vq,
	};

	bdev.compat_id = compat__add_message("virtio-balloon device was not detected",
						"While you have requested a virtio-balloon device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_VIRTIO_BALLOON.");
}
