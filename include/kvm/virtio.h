#ifndef KVM__VIRTIO_H
#define KVM__VIRTIO_H

#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>

#include <linux/types.h>
#include <sys/uio.h>

#include "kvm/kvm.h"

#define VIRTIO_IRQ_LOW		0
#define VIRTIO_IRQ_HIGH		1

#define VIRTIO_PCI_O_CONFIG	0
#define VIRTIO_PCI_O_MSIX	1

struct virt_queue {
	struct vring	vring;
	u32		pfn;
	/* The last_avail_idx field is an index to ->ring of struct vring_avail.
	   It's where we assume the next request index is at.  */
	u16		last_avail_idx;
	u16		last_used_signalled;
};

static inline u16 virt_queue__pop(struct virt_queue *queue)
{
	return queue->vring.avail->ring[queue->last_avail_idx++ % queue->vring.num];
}

static inline struct vring_desc *virt_queue__get_desc(struct virt_queue *queue, u16 desc_ndx)
{
	return &queue->vring.desc[desc_ndx];
}

static inline bool virt_queue__available(struct virt_queue *vq)
{
	if (!vq->vring.avail)
		return 0;

	vring_avail_event(&vq->vring) = vq->last_avail_idx;
	return vq->vring.avail->idx !=  vq->last_avail_idx;
}

/*
 * Warning: on 32-bit hosts, shifting pfn left may cause a truncation of pfn values
 * higher than 4GB - thus, pointing to the wrong area in guest virtual memory space
 * and breaking the virt queue which owns this pfn.
 */
static inline void *guest_pfn_to_host(struct kvm *kvm, u32 pfn)
{
	return guest_flat_to_host(kvm, (unsigned long)pfn << VIRTIO_PCI_QUEUE_ADDR_SHIFT);
}


struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, u32 head, u32 len);

bool virtio_queue__should_signal(struct virt_queue *vq);
u16 virt_queue__get_iov(struct virt_queue *vq, struct iovec iov[],
			u16 *out, u16 *in, struct kvm *kvm);
u16 virt_queue__get_head_iov(struct virt_queue *vq, struct iovec iov[],
			     u16 *out, u16 *in, u16 head, struct kvm *kvm);
u16 virt_queue__get_inout_iov(struct kvm *kvm, struct virt_queue *queue,
			      struct iovec in_iov[], struct iovec out_iov[],
			      u16 *in, u16 *out);
int virtio__get_dev_specific_field(int offset, bool msix, u32 *config_off);

enum virtio_trans {
	VIRTIO_PCI,
	VIRTIO_MMIO,
};

struct virtio_device {
	bool			use_vhost;
	void			*virtio;
	struct virtio_ops	*ops;
};

struct virtio_ops {
	u8 *(*get_config)(struct kvm *kvm, void *dev);
	u32 (*get_host_features)(struct kvm *kvm, void *dev);
	void (*set_guest_features)(struct kvm *kvm, void *dev, u32 features);
	int (*init_vq)(struct kvm *kvm, void *dev, u32 vq, u32 pfn);
	int (*notify_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_pfn_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_size_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*set_size_vq)(struct kvm *kvm, void *dev, u32 vq, int size);
	void (*notify_vq_gsi)(struct kvm *kvm, void *dev, u32 vq, u32 gsi);
	void (*notify_vq_eventfd)(struct kvm *kvm, void *dev, u32 vq, u32 efd);
	int (*signal_vq)(struct kvm *kvm, struct virtio_device *vdev, u32 queueid);
	int (*signal_config)(struct kvm *kvm, struct virtio_device *vdev);
	int (*init)(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		    int device_id, int subsys_id, int class);
	int (*exit)(struct kvm *kvm, struct virtio_device *vdev);
};

int virtio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		struct virtio_ops *ops, enum virtio_trans trans,
		int device_id, int subsys_id, int class);
int virtio_compat_add_message(const char *device, const char *config);
#endif /* KVM__VIRTIO_H */
