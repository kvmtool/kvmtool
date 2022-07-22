#include "kvm/virtio-rng.h"

#include "kvm/virtio-pci-dev.h"

#include "kvm/virtio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/threadpool.h"
#include "kvm/guest_compat.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_rng.h>

#include <linux/list.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <linux/kernel.h>

#define NUM_VIRT_QUEUES		1
#define VIRTIO_RNG_QUEUE_SIZE	128

struct rng_dev_job {
	struct virt_queue	*vq;
	struct rng_dev		*rdev;
	struct thread_pool__job	job_id;
};

struct rng_dev {
	struct list_head	list;
	struct virtio_device	vdev;

	int			fd;

	/* virtio queue */
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct rng_dev_job	jobs[NUM_VIRT_QUEUES];
};

static LIST_HEAD(rdevs);
static int compat_id = -1;

static u8 *get_config(struct kvm *kvm, void *dev)
{
	/* Unused */
	return 0;
}

static size_t get_config_size(struct kvm *kvm, void *dev)
{
	return 0;
}

static u64 get_host_features(struct kvm *kvm, void *dev)
{
	/* Unused */
	return 0;
}

static bool virtio_rng_do_io_request(struct kvm *kvm, struct rng_dev *rdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_RNG_QUEUE_SIZE];
	ssize_t len = 0;
	u16 out, in, head;

	head	= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	len	= readv(rdev->fd, iov, in);
	if (len < 0 && errno == EAGAIN)
		len = 0;

	virt_queue__set_used_elem(queue, head, len);

	return true;
}

static void virtio_rng_do_io(struct kvm *kvm, void *param)
{
	struct rng_dev_job *job	= param;
	struct virt_queue *vq	= job->vq;
	struct rng_dev *rdev	= job->rdev;

	while (virt_queue__available(vq))
		virtio_rng_do_io_request(kvm, rdev, vq);

	rdev->vdev.ops->signal_vq(kvm, &rdev->vdev, vq - rdev->vqs);
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct rng_dev *rdev = dev;
	struct virt_queue *queue;
	struct rng_dev_job *job;

	compat__remove_message(compat_id);

	queue		= &rdev->vqs[vq];

	job = &rdev->jobs[vq];

	virtio_init_device_vq(kvm, &rdev->vdev, queue, VIRTIO_RNG_QUEUE_SIZE);

	*job = (struct rng_dev_job) {
		.vq	= queue,
		.rdev	= rdev,
	};

	thread_pool__init_job(&job->job_id, kvm, virtio_rng_do_io, job);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct rng_dev *rdev = dev;

	thread_pool__do_job(&rdev->jobs[vq].job_id);

	return 0;
}

static struct virt_queue *get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct rng_dev *rdev = dev;

	return &rdev->vqs[vq];
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_RNG_QUEUE_SIZE;
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

static struct virtio_ops rng_dev_virtio_ops = {
	.get_config		= get_config,
	.get_config_size	= get_config_size,
	.get_host_features	= get_host_features,
	.init_vq		= init_vq,
	.notify_vq		= notify_vq,
	.get_vq			= get_vq,
	.get_size_vq		= get_size_vq,
	.set_size_vq		= set_size_vq,
	.get_vq_count		= get_vq_count,
};

int virtio_rng__init(struct kvm *kvm)
{
	struct rng_dev *rdev;
	int r;

	if (!kvm->cfg.virtio_rng)
		return 0;

	rdev = calloc(1, sizeof(*rdev));
	if (rdev == NULL)
		return -ENOMEM;

	rdev->fd = open("/dev/random", O_RDONLY | O_NONBLOCK);
	if (rdev->fd < 0) {
		r = rdev->fd;
		goto cleanup;
	}

	r = virtio_init(kvm, rdev, &rdev->vdev, &rng_dev_virtio_ops,
			VIRTIO_DEFAULT_TRANS(kvm), PCI_DEVICE_ID_VIRTIO_RNG,
			VIRTIO_ID_RNG, PCI_CLASS_RNG);
	if (r < 0)
		goto cleanup;

	list_add_tail(&rdev->list, &rdevs);

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-rng", "CONFIG_HW_RANDOM_VIRTIO");
	return 0;
cleanup:
	close(rdev->fd);
	free(rdev);

	return r;
}
virtio_dev_init(virtio_rng__init);

int virtio_rng__exit(struct kvm *kvm)
{
	struct rng_dev *rdev, *tmp;

	list_for_each_entry_safe(rdev, tmp, &rdevs, list) {
		list_del(&rdev->list);
		rdev->vdev.ops->exit(kvm, &rdev->vdev);
		free(rdev);
	}

	return 0;
}
virtio_dev_exit(virtio_rng__exit);
