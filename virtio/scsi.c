#include "kvm/virtio-scsi.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/disk-image.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/ioeventfd.h"
#include "kvm/guest_compat.h"
#include "kvm/virtio-pci.h"
#include "kvm/virtio.h"

#include <linux/kernel.h>
#include <linux/virtio_scsi.h>
#include <linux/vhost.h>

#define VIRTIO_SCSI_QUEUE_SIZE		128
#define NUM_VIRT_QUEUES			3

static LIST_HEAD(sdevs);
static int compat_id = -1;

struct scsi_dev {
	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct virtio_scsi_config	config;
	struct vhost_scsi_target	target;
	u32				features;
	int				vhost_fd;
	struct virtio_device		vdev;
	struct list_head		list;
	struct kvm			*kvm;
};

static u8 *get_config(struct kvm *kvm, void *dev)
{
	struct scsi_dev *sdev = dev;

	return ((u8 *)(&sdev->config));
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return	1UL << VIRTIO_RING_F_EVENT_IDX |
		1UL << VIRTIO_RING_F_INDIRECT_DESC;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	struct scsi_dev *sdev = dev;

	sdev->features = features;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 pfn)
{
	struct vhost_vring_state state = { .index = vq };
	struct vhost_vring_addr addr;
	struct scsi_dev *sdev = dev;
	struct virt_queue *queue;
	void *p;
	int r;

	compat__remove_message(compat_id);

	queue		= &sdev->vqs[vq];
	queue->pfn	= pfn;
	p		= guest_pfn_to_host(kvm, queue->pfn);

	vring_init(&queue->vring, VIRTIO_SCSI_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

	if (sdev->vhost_fd == 0)
		return 0;

	state.num = queue->vring.num;
	r = ioctl(sdev->vhost_fd, VHOST_SET_VRING_NUM, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_NUM failed");
	state.num = 0;
	r = ioctl(sdev->vhost_fd, VHOST_SET_VRING_BASE, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_BASE failed");

	addr = (struct vhost_vring_addr) {
		.index = vq,
		.desc_user_addr = (u64)(unsigned long)queue->vring.desc,
		.avail_user_addr = (u64)(unsigned long)queue->vring.avail,
		.used_user_addr = (u64)(unsigned long)queue->vring.used,
	};

	r = ioctl(sdev->vhost_fd, VHOST_SET_VRING_ADDR, &addr);
	if (r < 0)
		die_perror("VHOST_SET_VRING_ADDR failed");

	return 0;
}

static void notify_vq_gsi(struct kvm *kvm, void *dev, u32 vq, u32 gsi)
{
	struct vhost_vring_file file;
	struct scsi_dev *sdev = dev;
	struct kvm_irqfd irq;
	int r;

	if (sdev->vhost_fd == 0)
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

	r = ioctl(sdev->vhost_fd, VHOST_SET_VRING_CALL, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_CALL failed");

	if (vq > 0)
		return;

	r = ioctl(sdev->vhost_fd, VHOST_SCSI_SET_ENDPOINT, &sdev->target);
	if (r != 0)
		die("VHOST_SCSI_SET_ENDPOINT failed %d", errno);
}

static void notify_vq_eventfd(struct kvm *kvm, void *dev, u32 vq, u32 efd)
{
	struct scsi_dev *sdev = dev;
	struct vhost_vring_file file = {
		.index	= vq,
		.fd	= efd,
	};
	int r;

	if (sdev->vhost_fd == 0)
		return;

	r = ioctl(sdev->vhost_fd, VHOST_SET_VRING_KICK, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_KICK failed");
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct scsi_dev *sdev = dev;

	return sdev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_SCSI_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	return size;
}

static struct virtio_ops scsi_dev_virtio_ops = (struct virtio_ops) {
	.get_config		= get_config,
	.get_host_features	= get_host_features,
	.set_guest_features	= set_guest_features,
	.init_vq		= init_vq,
	.get_pfn_vq		= get_pfn_vq,
	.get_size_vq		= get_size_vq,
	.set_size_vq		= set_size_vq,
	.notify_vq		= notify_vq,
	.notify_vq_gsi		= notify_vq_gsi,
	.notify_vq_eventfd	= notify_vq_eventfd,
};

static void virtio_scsi_vhost_init(struct kvm *kvm, struct scsi_dev *sdev)
{
	struct vhost_memory *mem;
	u64 features;
	int r;

	sdev->vhost_fd = open("/dev/vhost-scsi", O_RDWR);
	if (sdev->vhost_fd < 0)
		die_perror("Failed openning vhost-scsi device");

	mem = calloc(1, sizeof(*mem) + sizeof(struct vhost_memory_region));
	if (mem == NULL)
		die("Failed allocating memory for vhost memory map");

	mem->nregions = 1;
	mem->regions[0] = (struct vhost_memory_region) {
		.guest_phys_addr	= 0,
		.memory_size		= kvm->ram_size,
		.userspace_addr		= (unsigned long)kvm->ram_start,
	};

	r = ioctl(sdev->vhost_fd, VHOST_SET_OWNER);
	if (r != 0)
		die_perror("VHOST_SET_OWNER failed");

	r = ioctl(sdev->vhost_fd, VHOST_GET_FEATURES, &features);
	if (r != 0)
		die_perror("VHOST_GET_FEATURES failed");

	r = ioctl(sdev->vhost_fd, VHOST_SET_FEATURES, &features);
	if (r != 0)
		die_perror("VHOST_SET_FEATURES failed");
	r = ioctl(sdev->vhost_fd, VHOST_SET_MEM_TABLE, mem);
	if (r != 0)
		die_perror("VHOST_SET_MEM_TABLE failed");

	sdev->vdev.use_vhost = true;

	free(mem);
}


static int virtio_scsi_init_one(struct kvm *kvm, struct disk_image *disk)
{
	struct scsi_dev *sdev;

	if (!disk)
		return -EINVAL;

	sdev = calloc(1, sizeof(struct scsi_dev));
	if (sdev == NULL)
		return -ENOMEM;

	*sdev = (struct scsi_dev) {
		.config	= (struct virtio_scsi_config) {
			.num_queues	= NUM_VIRT_QUEUES - 2,
			.seg_max	= VIRTIO_SCSI_CDB_SIZE - 2,
			.max_sectors	= 65535,
			.cmd_per_lun	= 128,
			.sense_size	= VIRTIO_SCSI_SENSE_SIZE,
			.cdb_size	= VIRTIO_SCSI_CDB_SIZE,
			.max_channel	= 0,
			.max_target	= 0,
			.max_lun	= 16383,
			.event_info_size = sizeof(struct virtio_scsi_event),
		},
		.kvm			= kvm,
	};
	strncpy((char *)&sdev->target.vhost_wwpn, disk->wwpn, sizeof(sdev->target.vhost_wwpn));
	sdev->target.vhost_tpgt = strtol(disk->tpgt, NULL, 0);

	virtio_init(kvm, sdev, &sdev->vdev, &scsi_dev_virtio_ops,
		    VIRTIO_DEFAULT_TRANS, PCI_DEVICE_ID_VIRTIO_SCSI,
		    VIRTIO_ID_SCSI, PCI_CLASS_BLK);

	list_add_tail(&sdev->list, &sdevs);

	virtio_scsi_vhost_init(kvm, sdev);

	if (compat_id == -1)
		compat_id = virtio_compat_add_message("virtio-scsi", "CONFIG_VIRTIO_SCSI");

	return 0;
}

static int virtio_scsi_exit_one(struct kvm *kvm, struct scsi_dev *sdev)
{
	int r;

	r = ioctl(sdev->vhost_fd, VHOST_SCSI_CLEAR_ENDPOINT, &sdev->target);
	if (r != 0)
		die("VHOST_SCSI_CLEAR_ENDPOINT failed %d", errno);

	list_del(&sdev->list);
	free(sdev);

	return 0;
}

int virtio_scsi_init(struct kvm *kvm)
{
	int i, r = 0;

	for (i = 0; i < kvm->nr_disks; i++) {
		if (!kvm->disks[i]->wwpn)
			continue;
		r = virtio_scsi_init_one(kvm, kvm->disks[i]);
		if (r < 0)
			goto cleanup;
	}

	return 0;
cleanup:
	return virtio_scsi_exit(kvm);
}
virtio_dev_init(virtio_scsi_init);

int virtio_scsi_exit(struct kvm *kvm)
{
	while (!list_empty(&sdevs)) {
		struct scsi_dev *sdev;

		sdev = list_first_entry(&sdevs, struct scsi_dev, list);
		virtio_scsi_exit_one(kvm, sdev);
	}

	return 0;
}
virtio_dev_exit(virtio_scsi_exit);
