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
	return 1UL << VIRTIO_RING_F_EVENT_IDX
		| 1UL << VIRTIO_RING_F_INDIRECT_DESC;
}

static bool is_event_vq(u32 vq)
{
	return vq == VSOCK_VQ_EVENT;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct vhost_vring_state state = { .index = vq };
	struct vhost_vring_addr addr;
	struct vsock_dev *vdev = dev;
	struct virt_queue *queue;
	int r;

	compat__remove_message(compat_id);

	queue		= &vdev->vqs[vq];
	virtio_init_device_vq(kvm, &vdev->vdev, queue, VIRTIO_VSOCK_QUEUE_SIZE);

	if (vdev->vhost_fd == -1)
		return 0;

	if (is_event_vq(vq))
		return 0;

	state.num = queue->vring.num;
	r = ioctl(vdev->vhost_fd, VHOST_SET_VRING_NUM, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_NUM failed");

	state.num = 0;
	r = ioctl(vdev->vhost_fd, VHOST_SET_VRING_BASE, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_BASE failed");

	addr = (struct vhost_vring_addr) {
		.index = vq,
		.desc_user_addr = (u64)(unsigned long)queue->vring.desc,
		.avail_user_addr = (u64)(unsigned long)queue->vring.avail,
		.used_user_addr = (u64)(unsigned long)queue->vring.used,
	};

	r = ioctl(vdev->vhost_fd, VHOST_SET_VRING_ADDR, &addr);
	if (r < 0)
		die_perror("VHOST_SET_VRING_ADDR failed");

	return 0;
}

static void notify_vq_eventfd(struct kvm *kvm, void *dev, u32 vq, u32 efd)
{
	struct vsock_dev *vdev = dev;
	struct vhost_vring_file file = {
		.index	= vq,
		.fd	= efd,
	};
	int r;

	if (is_event_vq(vq))
		return;

	if (vdev->vhost_fd == -1)
		return;

	r = ioctl(vdev->vhost_fd, VHOST_SET_VRING_KICK, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_KICK failed");
}

static void notify_status(struct kvm *kvm, void *dev, u32 status)
{
	struct vsock_dev *vdev = dev;
	int r, start;

	if (status & VIRTIO__STATUS_CONFIG)
		vdev->config.guest_cid = cpu_to_le64(vdev->guest_cid);

	if (status & VIRTIO__STATUS_START)
		start = 1;
	else if (status & VIRTIO__STATUS_STOP)
		start = 0;
	else
		return;

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
	struct vhost_vring_file file;
	struct vsock_dev *vdev = dev;
	struct kvm_irqfd irq;
	int r;

	if (vdev->vhost_fd == -1)
		return;

	if (is_event_vq(vq))
		return;

	irq = (struct kvm_irqfd) {
		.gsi	= gsi,
		.fd	= eventfd(0, 0),
	};
	file = (struct vhost_vring_file) {
		.index	= vq,
		.fd	= irq.fd,
	};

	r = ioctl(kvm->vm_fd, KVM_IRQFD, &irq);
	if (r < 0)
		die_perror("KVM_IRQFD failed");

	r = ioctl(vdev->vhost_fd, VHOST_SET_VRING_CALL, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_CALL failed");
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
	struct kvm_mem_bank *bank;
	struct vhost_memory *mem;
	u64 features;
	int r, i;

	vdev->vhost_fd = open("/dev/vhost-vsock", O_RDWR);
	if (vdev->vhost_fd < 0)
		die_perror("Failed opening vhost-vsock device");

	mem = calloc(1, sizeof(*mem) + sizeof(struct vhost_memory_region));
	if (mem == NULL)
		die("Failed allocating memory for vhost memory map");

	i = 0;
	list_for_each_entry(bank, &kvm->mem_banks, list) {
		mem->regions[i] = (struct vhost_memory_region) {
			.guest_phys_addr = bank->guest_phys_addr,
			.memory_size	 = bank->size,
			.userspace_addr	 = (unsigned long)bank->host_addr,
		};
		i++;
	}
	mem->nregions = i;

	r = ioctl(vdev->vhost_fd, VHOST_SET_OWNER);
	if (r != 0)
		die_perror("VHOST_SET_OWNER failed");

	r = ioctl(vdev->vhost_fd, VHOST_SET_MEM_TABLE, mem);
	if (r != 0)
		die_perror("VHOST_SET_MEM_TABLE failed");

	r = ioctl(vdev->vhost_fd, VHOST_GET_FEATURES, &features);
	if (r != 0)
		die_perror("VHOST_GET_FEATURES failed");

	r = ioctl(vdev->vhost_fd, VHOST_SET_FEATURES, &features);
	if (r != 0)
		die_perror("VHOST_SET_FEATURES failed");

	r = ioctl(vdev->vhost_fd, VHOST_VSOCK_SET_GUEST_CID, &vdev->guest_cid);
	if (r != 0)
		die_perror("VHOST_VSOCK_SET_GUEST_CID failed");

	vdev->vdev.use_vhost = true;

	free(mem);
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
		    VIRTIO_DEFAULT_TRANS(kvm), PCI_DEVICE_ID_VIRTIO_VSOCK,
		    VIRTIO_ID_VSOCK, PCI_CLASS_VSOCK);
	if (r < 0)
	    return r;

	virtio_vhost_vsock_init(kvm, vdev);

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-vsock", "CONFIG_VIRTIO_VSOCK");

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
