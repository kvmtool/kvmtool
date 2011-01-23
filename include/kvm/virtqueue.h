#ifndef KVM__VIRTQUEUE_H
#define KVM__VIRTQUEUE_H

#include <linux/virtio_ring.h>

#include <stdint.h>

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

#endif /* KVM__VIRTQUEUE_H */
