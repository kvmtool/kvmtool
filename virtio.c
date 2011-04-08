#include <linux/virtio_ring.h>
#include <stdint.h>
#include <sys/uio.h>
#include "kvm/kvm.h"
#include "kvm/virtio.h"

struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, uint32_t head, uint32_t len)
{
	struct vring_used_elem *used_elem;
	used_elem	= &queue->vring.used->ring[queue->vring.used->idx++ % queue->vring.num];
	used_elem->id	= head;
	used_elem->len	= len;
	return used_elem;
}

uint16_t virt_queue__get_iov(struct virt_queue *queue, struct iovec iov[], uint16_t *out, uint16_t *in, struct kvm *kvm)
{
	struct vring_desc *desc;
	uint16_t head, idx;

	idx = head = virt_queue__pop(queue);
	*out = *in = 0;

	do {
		desc				= virt_queue__get_desc(queue, idx);
		iov[*out + *in].iov_base	= guest_flat_to_host(kvm, desc->addr);
		iov[*out + *in].iov_len		= desc->len;
		if (desc->flags & VRING_DESC_F_WRITE)
			(*in)++;
		else
			(*out)++;
		if (desc->flags & VRING_DESC_F_NEXT)
			idx = desc->next;
		else
			break;
	} while (1);

	return head;
}
