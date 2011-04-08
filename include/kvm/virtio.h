#ifndef KVM__VIRTIO_H
#define KVM__VIRTIO_H

#include <linux/virtio_ring.h>

#include <stdint.h>
#include <sys/uio.h>

#include "kvm/kvm.h"

struct virt_queue {
	struct vring			vring;
	uint32_t			pfn;
	/* The last_avail_idx field is an index to ->ring of struct vring_avail.
	   It's where we assume the next request index is at.  */
	uint16_t			last_avail_idx;
};

static inline uint16_t virt_queue__pop(struct virt_queue *queue)
{
	return queue->vring.avail->ring[queue->last_avail_idx++ % queue->vring.num];
}

static inline struct vring_desc *virt_queue__get_desc(struct virt_queue *queue, uint16_t desc_ndx)
{
	return &queue->vring.desc[desc_ndx];
}

static inline struct vring_used_elem *virt_queue__get_used_elem(struct virt_queue *queue)
{
	return &queue->vring.used->ring[queue->vring.used->idx++ % queue->vring.num];
}


static inline bool virt_queue__available(struct virt_queue *vq)
{
	return vq->vring.avail->idx !=  vq->last_avail_idx;
}

struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, uint32_t head, uint32_t len);

uint16_t virt_queue__get_iov(struct virt_queue *queue, struct iovec iov[], uint16_t *out, uint16_t *in, struct kvm *kvm);

#endif /* KVM__VIRTIO_H */
