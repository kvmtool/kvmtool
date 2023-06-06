#include "kvm/virtio-vsock.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/ioeventfd.h"
#include "kvm/guest_compat.h"
#include "kvm/virtio-pci.h"
#include "kvm/virtio.h"

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/virtio_vsock.h>
#include <linux/vhost.h>

#define VIRTIO_VSOCK_QUEUE_SIZE		128

static LIST_HEAD(vdevs);
static int compat_id = -1;

enum {
	VSOCK_VQ_RX     = 0, /* for host to guest data */
	VSOCK_VQ_TX     = 1, /* for guest to host data */
	VSOCK_VQ_EVENT  = 2,
	VSOCK_VQ_MAX    = 3,
};

struct vsock_dev {
	struct virt_queue		vqs[VSOCK_VQ_MAX];
	struct virtio_vsock_config	config;
	u64				guest_cid;
	u32				features;
	int				vhost_fd;
	struct virtio_device		vdev;
	struct list_head		list;
	struct kvm			*kvm;
};

static u8 *get_config(struct kvm *kvm, void *dev)
{
	struct vsock_dev *vdev = dev;

	return ((u8 *)(&vdev->config));
}

static size_t get_config_size(struct kvm *kvm, void *dev)
{
	struct vsock_dev *vdev = dev;

	return sizeof(vdev->config);
}

static u64 get_host_features(struct kvm *kvm, void *dev)
{
	int r;
	u64 features;
	struct vsock_dev *vdev = dev;

	r = ioctl(vdev->vhost_fd, VHOST_GET_FEATURES, &features);
	if (r != 0)
		die_perror("VHOST_GET_FEATURES failed");

	return features &
		(1ULL << VIRTIO_RING_F_EVENT_IDX |
		 1ULL << VIRTIO_RING_F_INDIRECT_DESC);
}

static bool is_event_vq(u32 vq)
{
	return vq == VSOCK_VQ_EVENT;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct vsock_dev *vdev = dev;
	struct virt_queue *queue;

	compat__remove_message(compat_id);

	queue		= &vdev->vqs[vq];
	virtio_init_device_vq(kvm, &vdev->vdev, queue, VIRTIO_VSOCK_QUEUE_SIZE);

	if (vdev->vhost_fd == -1 || is_event_vq(vq))
		return 0;

	virtio_vhost_set_vring(kvm, vdev->vhost_fd, vq, queue);
	return 0;
}

static void notify_vq_eventfd(struct kvm *kvm, void *dev, u32 vq, u32 efd)
{
	struct vsock_dev *vdev = dev;

	if (vdev->vhost_fd == -1 || is_event_vq(vq))
		return;

	virtio_vhost_set_vring_kick(kvm, vdev->vhost_fd, vq, efd);
}

static void notify_status(struct kvm *kvm, void *dev, u32 status)
{
	struct vsock_dev *vdev = dev;
	int r, start;

	if (status & VIRTIO__STATUS_CONFIG)
		vdev->config.guest_cid = cpu_to_le64(vdev->guest_cid);

	if (status & VIRTIO__STATUS_START) {
		start = 1;

		r = virtio_vhost_set_features(vdev->vhost_fd,
					      vdev->vdev.features);
		if (r != 0)
			die_perror("VHOST_SET_FEATURES failed");
	} else if (status & VIRTIO__STATUS_STOP) {
		start = 0;
	} else {
		return;
	}

	r = ioctl(vdev->vhost_fd, VHOST_VSOCK_SET_RUNNING, &start);
	if (r != 0)
		die("VHOST_VSOCK_SET_RUNNING failed %d", errno);
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return 0;
}

static struct virt_queue *get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct vsock_dev *vdev = dev;

	return &vdev->vqs[vq];
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_VSOCK_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	return size;
}

static void notify_vq_gsi(struct kvm *kvm, void *dev, u32 vq, u32 gsi)
{
	struct vsock_dev *vdev = dev;

	if (vdev->vhost_fd == -1 || is_event_vq(vq))
		return;

	virtio_vhost_set_vring_irqfd(kvm, gsi, &vdev->vqs[vq]);
}

static unsigned int get_vq_count(struct kvm *kvm, void *dev)
{
	return VSOCK_VQ_MAX;
}

static struct virtio_ops vsock_dev_virtio_ops = {
	.get_config		= get_config,
	.get_config_size	= get_config_size,
	.get_host_features	= get_host_features,
	.init_vq		= init_vq,
	.get_vq			= get_vq,
	.get_size_vq		= get_size_vq,
	.set_size_vq		= set_size_vq,
	.notify_vq_eventfd	= notify_vq_eventfd,
	.notify_status		= notify_status,
	.notify_vq_gsi		= notify_vq_gsi,
	.notify_vq		= notify_vq,
	.get_vq_count		= get_vq_count,
};

static void virtio_vhost_vsock_init(struct kvm *kvm, struct vsock_dev *vdev)
{
	int r;

	vdev->vhost_fd = open("/dev/vhost-vsock", O_RDWR);
	if (vdev->vhost_fd < 0)
		die_perror("Failed opening vhost-vsock device");

	virtio_vhost_init(kvm, vdev->vhost_fd);

	r = ioctl(vdev->vhost_fd, VHOST_VSOCK_SET_GUEST_CID, &vdev->guest_cid);
	if (r != 0)
		die_perror("VHOST_VSOCK_SET_GUEST_CID failed");

	vdev->vdev.use_vhost = true;
}

static int virtio_vsock_init_one(struct kvm *kvm, u64 guest_cid)
{
	struct vsock_dev *vdev;
	int r;

	vdev = calloc(1, sizeof(struct vsock_dev));
	if (vdev == NULL)
		return -ENOMEM;

	*vdev = (struct vsock_dev) {
		.guest_cid		= guest_cid,
		.vhost_fd		= -1,
		.kvm			= kvm,
	};

	list_add_tail(&vdev->list, &vdevs);

	r = virtio_init(kvm, vdev, &vdev->vdev, &vsock_dev_virtio_ops,
		    kvm->cfg.virtio_transport, PCI_DEVICE_ID_VIRTIO_VSOCK,
		    VIRTIO_ID_VSOCK, PCI_CLASS_VSOCK);
	if (r < 0)
	    return r;

	virtio_vhost_vsock_init(kvm, vdev);

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-vsock", "CONFIG_VIRTIO_VSOCKETS");

	return 0;
}

static int virtio_vsock_exit_one(struct kvm *kvm, struct vsock_dev *vdev)
{
	list_del(&vdev->list);
	free(vdev);

	return 0;
}

int virtio_vsock_init(struct kvm *kvm)
{
	int r;

	if (kvm->cfg.vsock_cid == 0)
		return 0;

	r = virtio_vsock_init_one(kvm, kvm->cfg.vsock_cid);
	if (r < 0)
		goto cleanup;

	return 0;
cleanup:
	return virtio_vsock_exit(kvm);
}
virtio_dev_init(virtio_vsock_init);

int virtio_vsock_exit(struct kvm *kvm)
{
	while (!list_empty(&vdevs)) {
		struct vsock_dev *vdev;

		vdev = list_first_entry(&vdevs, struct vsock_dev, list);
		virtio_vsock_exit_one(kvm, vdev);
	}

	return 0;
}
virtio_dev_exit(virtio_vsock_exit);
