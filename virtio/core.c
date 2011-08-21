#include <linux/virtio_ring.h>
#include <linux/types.h>
#include <sys/uio.h>

#include "kvm/barrier.h"

#include "kvm/kvm.h"
#include "kvm/virtio.h"

struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, u32 head, u32 len)
{
	struct vring_used_elem *used_elem;

	used_elem	= &queue->vring.used->ring[queue->vring.used->idx % queue->vring.num];
	used_elem->id	= head;
	used_elem->len	= len;

	/*
	 * Use wmb to assure that used elem was updated with head and len.
	 * We need a wmb here since we can't advance idx unless we're ready
	 * to pass the used element to the guest.
	 */
	wmb();
	queue->vring.used->idx++;

	/*
	 * Use wmb to assure used idx has been increased before we signal the guest.
	 * Without a wmb here the guest may ignore the queue since it won't see
	 * an updated idx.
	 */
	wmb();

	return used_elem;
}

u16 virt_queue__get_iov(struct virt_queue *queue, struct iovec iov[], u16 *out, u16 *in, struct kvm *kvm)
{
	struct vring_desc *desc;
	u16 head, idx;

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

/* in and out are relative to guest */
u16 virt_queue__get_inout_iov(struct kvm *kvm, struct virt_queue *queue,
			      struct iovec in_iov[], struct iovec out_iov[],
			      u16 *in, u16 *out)
{
	u16 head, idx;
	struct vring_desc *desc;

	idx = head = virt_queue__pop(queue);
	*out = *in = 0;
	do {
		desc = virt_queue__get_desc(queue, idx);
		if (desc->flags & VRING_DESC_F_WRITE) {
			in_iov[*in].iov_base = guest_flat_to_host(kvm,
								  desc->addr);
			in_iov[*in].iov_len = desc->len;
			(*in)++;
		} else {
			out_iov[*out].iov_base = guest_flat_to_host(kvm,
								    desc->addr);
			out_iov[*out].iov_len = desc->len;
			(*out)++;
		}
		if (desc->flags & VRING_DESC_F_NEXT)
			idx = desc->next;
		else
			break;
	} while (1);
	return head;
}


void virt_queue__trigger_irq(struct virt_queue *vq, int irq, u8 *isr, struct kvm *kvm)
{
	if (vq->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)
		return;

	if (*isr == VIRTIO_IRQ_LOW) {
		*isr = VIRTIO_IRQ_HIGH;
		kvm__irq_line(kvm, irq, VIRTIO_IRQ_HIGH);
	}
}

int virtio__get_dev_specific_field(int offset, bool msix, bool features_hi, u32 *config_off)
{
	if (msix) {
		if (offset < 4)
			return VIRTIO_PCI_O_MSIX;
		else
			offset -= 4;
	}

	if (features_hi) {
		if (offset < 4)
			return VIRTIO_PCI_O_FEATURES;
		else
			offset -= 4;
	}

	*config_off = offset;

	return VIRTIO_PCI_O_CONFIG;
}
