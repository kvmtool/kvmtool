#ifndef KVM__VIRTIO_H
#define KVM__VIRTIO_H

#include <linux/virtio_ring.h>

#include <linux/types.h>
#include <sys/uio.h>

#include "kvm/kvm.h"

struct virt_queue {
	struct vring	vring;
	u32		pfn;
	/* The last_avail_idx field is an index to ->ring of struct vring_avail.
	   It's where we assume the next request index is at.  */
	u16		last_avail_idx;
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
	return vq->vring.avail->idx !=  vq->last_avail_idx;
}

struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, u32 head, u32 len);

u16 virt_queue__get_iov(struct virt_queue *queue, struct iovec iov[], u16 *out, u16 *in, struct kvm *kvm);

#endif /* KVM__VIRTIO_H */
