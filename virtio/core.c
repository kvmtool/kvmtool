#include <linux/virtio_ring.h>
#include <linux/types.h>
#include <sys/uio.h>
#include <stdlib.h>

#include "kvm/guest_compat.h"
#include "kvm/barrier.h"
#include "kvm/virtio.h"
#include "kvm/virtio-pci.h"
#include "kvm/virtio-mmio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"


const char* virtio_trans_name(enum virtio_trans trans)
{
	if (trans == VIRTIO_PCI || trans == VIRTIO_PCI_LEGACY)
		return "pci";
	else if (trans == VIRTIO_MMIO || trans == VIRTIO_MMIO_LEGACY)
		return "mmio";
	return "unknown";
}

void virt_queue__used_idx_advance(struct virt_queue *queue, u16 jump)
{
	u16 idx = virtio_guest_to_host_u16(queue, queue->vring.used->idx);

	/*
	 * Use wmb to assure that used elem was updated with head and len.
	 * We need a wmb here since we can't advance idx unless we're ready
	 * to pass the used element to the guest.
	 */
	wmb();
	idx += jump;
	queue->vring.used->idx = virtio_host_to_guest_u16(queue, idx);
}

struct vring_used_elem *
virt_queue__set_used_elem_no_update(struct virt_queue *queue, u32 head,
				    u32 len, u16 offset)
{
	struct vring_used_elem *used_elem;
	u16 idx = virtio_guest_to_host_u16(queue, queue->vring.used->idx);

	idx += offset;
	used_elem	= &queue->vring.used->ring[idx % queue->vring.num];
	used_elem->id	= virtio_host_to_guest_u32(queue, head);
	used_elem->len	= virtio_host_to_guest_u32(queue, len);

	return used_elem;
}

struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, u32 head, u32 len)
{
	struct vring_used_elem *used_elem;

	used_elem = virt_queue__set_used_elem_no_update(queue, head, len, 0);
	virt_queue__used_idx_advance(queue, 1);

	return used_elem;
}

static inline bool virt_desc__test_flag(struct virt_queue *vq,
					struct vring_desc *desc, u16 flag)
{
	return !!(virtio_guest_to_host_u16(vq, desc->flags) & flag);
}

/*
 * Each buffer in the virtqueues is actually a chain of descriptors.  This
 * function returns the next descriptor in the chain, or max if we're at the
 * end.
 */
static unsigned next_desc(struct virt_queue *vq, struct vring_desc *desc,
			  unsigned int i, unsigned int max)
{
	unsigned int next;

	/* If this descriptor says it doesn't chain, we're done. */
	if (!virt_desc__test_flag(vq, &desc[i], VRING_DESC_F_NEXT))
		return max;

	next = virtio_guest_to_host_u16(vq, desc[i].next);

	/* Ensure they're not leading us off end of descriptors. */
	return min(next, max);
}

u16 virt_queue__get_head_iov(struct virt_queue *vq, struct iovec iov[], u16 *out, u16 *in, u16 head, struct kvm *kvm)
{
	struct vring_desc *desc;
	u16 idx;
	u16 max;

	idx = head;
	*out = *in = 0;
	max = vq->vring.num;
	desc = vq->vring.desc;

	if (virt_desc__test_flag(vq, &desc[idx], VRING_DESC_F_INDIRECT)) {
		max = virtio_guest_to_host_u32(vq, desc[idx].len) / sizeof(struct vring_desc);
		desc = guest_flat_to_host(kvm, virtio_guest_to_host_u64(vq, desc[idx].addr));
		idx = 0;
	}

	do {
		/* Grab the first descriptor, and check it's OK. */
		iov[*out + *in].iov_len = virtio_guest_to_host_u32(vq, desc[idx].len);
		iov[*out + *in].iov_base = guest_flat_to_host(kvm,
							      virtio_guest_to_host_u64(vq, desc[idx].addr));
		/* If this is an input descriptor, increment that count. */
		if (virt_desc__test_flag(vq, &desc[idx], VRING_DESC_F_WRITE))
			(*in)++;
		else
			(*out)++;
	} while ((idx = next_desc(vq, desc, idx, max)) != max);

	return head;
}

u16 virt_queue__get_iov(struct virt_queue *vq, struct iovec iov[], u16 *out, u16 *in, struct kvm *kvm)
{
	u16 head;

	head = virt_queue__pop(vq);

	return virt_queue__get_head_iov(vq, iov, out, in, head, kvm);
}

/* in and out are relative to guest */
u16 virt_queue__get_inout_iov(struct kvm *kvm, struct virt_queue *queue,
			      struct iovec in_iov[], struct iovec out_iov[],
			      u16 *in, u16 *out)
{
	struct vring_desc *desc;
	u16 head, idx;

	idx = head = virt_queue__pop(queue);
	*out = *in = 0;
	do {
		u64 addr;
		desc = virt_queue__get_desc(queue, idx);
		addr = virtio_guest_to_host_u64(queue, desc->addr);
		if (virt_desc__test_flag(queue, desc, VRING_DESC_F_WRITE)) {
			in_iov[*in].iov_base = guest_flat_to_host(kvm, addr);
			in_iov[*in].iov_len = virtio_guest_to_host_u32(queue, desc->len);
			(*in)++;
		} else {
			out_iov[*out].iov_base = guest_flat_to_host(kvm, addr);
			out_iov[*out].iov_len = virtio_guest_to_host_u32(queue, desc->len);
			(*out)++;
		}
		if (virt_desc__test_flag(queue, desc, VRING_DESC_F_NEXT))
			idx = virtio_guest_to_host_u16(queue, desc->next);
		else
			break;
	} while (1);

	return head;
}

void virtio_init_device_vq(struct kvm *kvm, struct virtio_device *vdev,
			   struct virt_queue *vq, size_t nr_descs)
{
	struct vring_addr *addr = &vq->vring_addr;

	vq->endian		= vdev->endian;
	vq->use_event_idx	= (vdev->features & (1UL << VIRTIO_RING_F_EVENT_IDX));
	vq->enabled		= true;

	if (addr->legacy) {
		unsigned long base = (u64)addr->pfn * addr->pgsize;
		void *p = guest_flat_to_host(kvm, base);

		vring_init(&vq->vring, nr_descs, p, addr->align);
	} else {
		u64 desc = (u64)addr->desc_hi << 32 | addr->desc_lo;
		u64 avail = (u64)addr->avail_hi << 32 | addr->avail_lo;
		u64 used = (u64)addr->used_hi << 32 | addr->used_lo;

		vq->vring = (struct vring) {
			.desc	= guest_flat_to_host(kvm, desc),
			.used	= guest_flat_to_host(kvm, used),
			.avail	= guest_flat_to_host(kvm, avail),
			.num	= nr_descs,
		};
	}
}

void virtio_exit_vq(struct kvm *kvm, struct virtio_device *vdev,
			   void *dev, int num)
{
	struct virt_queue *vq = vdev->ops->get_vq(kvm, dev, num);

	if (vq->enabled && vdev->ops->exit_vq)
		vdev->ops->exit_vq(kvm, dev, num);
	memset(vq, 0, sizeof(*vq));
}

int virtio__get_dev_specific_field(int offset, bool msix, u32 *config_off)
{
	if (msix) {
		if (offset < 4)
			return VIRTIO_PCI_O_MSIX;
		else
			offset -= 4;
	}

	*config_off = offset;

	return VIRTIO_PCI_O_CONFIG;
}

bool virtio_queue__should_signal(struct virt_queue *vq)
{
	u16 old_idx, new_idx, event_idx;

	/*
	 * Use mb to assure used idx has been increased before we signal the
	 * guest, and we don't read a stale value for used_event. Without a mb
	 * here we might not send a notification that we need to send, or the
	 * guest may ignore the queue since it won't see an updated idx.
	 */
	mb();

	if (!vq->use_event_idx) {
		/*
		 * When VIRTIO_RING_F_EVENT_IDX isn't negotiated, interrupt the
		 * guest if it didn't explicitly request to be left alone.
		 */
		return !(virtio_guest_to_host_u16(vq, vq->vring.avail->flags) &
			 VRING_AVAIL_F_NO_INTERRUPT);
	}

	old_idx		= vq->last_used_signalled;
	new_idx		= virtio_guest_to_host_u16(vq, vq->vring.used->idx);
	event_idx	= virtio_guest_to_host_u16(vq, vring_used_event(&vq->vring));

	if (vring_need_event(event_idx, new_idx, old_idx)) {
		vq->last_used_signalled = new_idx;
		return true;
	}

	return false;
}

void virtio_set_guest_features(struct kvm *kvm, struct virtio_device *vdev,
			       void *dev, u64 features)
{
	/* TODO: fail negotiation if features & ~host_features */

	vdev->features |= features;
}

void virtio_notify_status(struct kvm *kvm, struct virtio_device *vdev,
			  void *dev, u8 status)
{
	u32 ext_status = status;

	vdev->status &= ~VIRTIO_CONFIG_S_MASK;
	vdev->status |= status;

	/* Add a few hints to help devices */
	if ((status & VIRTIO_CONFIG_S_DRIVER_OK) &&
	    !(vdev->status & VIRTIO__STATUS_START)) {
		vdev->status |= VIRTIO__STATUS_START;
		ext_status |= VIRTIO__STATUS_START;

	} else if (!status && (vdev->status & VIRTIO__STATUS_START)) {
		vdev->status &= ~VIRTIO__STATUS_START;
		ext_status |= VIRTIO__STATUS_STOP;

		/*
		 * Reset virtqueues and stop all traffic now, so that the device
		 * can safely reset the backend in notify_status().
		 */
		vdev->ops->reset(kvm, vdev);
	}
	if (!status)
		ext_status |= VIRTIO__STATUS_CONFIG;

	if (vdev->ops->notify_status)
		vdev->ops->notify_status(kvm, dev, ext_status);
}

bool virtio_access_config(struct kvm *kvm, struct virtio_device *vdev,
			  void *dev, unsigned long offset, void *data,
			  size_t size, bool is_write)
{
	void *in, *out, *config;
	size_t config_size = vdev->ops->get_config_size(kvm, dev);

	if (WARN_ONCE(offset + size > config_size,
		      "Config access offset (%lu) is beyond config size (%zu)\n",
		      offset, config_size))
		return false;

	config = vdev->ops->get_config(kvm, dev) + offset;

	in = is_write ? data : config;
	out = is_write ? config : data;

	switch (size) {
	case 1:
		*(u8 *)out = *(u8 *)in;
		break;
	case 2:
		*(u16 *)out = *(u16 *)in;
		break;
	case 4:
		*(u32 *)out = *(u32 *)in;
		break;
	case 8:
		*(u64 *)out = *(u64 *)in;
		break;
	default:
		WARN_ONCE(1, "%s: invalid access size\n", __func__);
		return false;
	}

	return true;
}

int virtio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		struct virtio_ops *ops, enum virtio_trans trans,
		int device_id, int subsys_id, int class)
{
	void *virtio;
	int r;

	switch (trans) {
	case VIRTIO_PCI_LEGACY:
		vdev->legacy			= true;
		/* fall through */
	case VIRTIO_PCI:
		virtio = calloc(sizeof(struct virtio_pci), 1);
		if (!virtio)
			return -ENOMEM;
		vdev->virtio			= virtio;
		vdev->ops			= ops;
		vdev->ops->signal_vq		= virtio_pci__signal_vq;
		vdev->ops->signal_config	= virtio_pci__signal_config;
		vdev->ops->init			= virtio_pci__init;
		vdev->ops->exit			= virtio_pci__exit;
		vdev->ops->reset		= virtio_pci__reset;
		r = vdev->ops->init(kvm, dev, vdev, device_id, subsys_id, class);
		break;
	case VIRTIO_MMIO_LEGACY:
		vdev->legacy			= true;
		/* fall through */
	case VIRTIO_MMIO:
		virtio = calloc(sizeof(struct virtio_mmio), 1);
		if (!virtio)
			return -ENOMEM;
		vdev->virtio			= virtio;
		vdev->ops			= ops;
		vdev->ops->signal_vq		= virtio_mmio_signal_vq;
		vdev->ops->signal_config	= virtio_mmio_signal_config;
		vdev->ops->init			= virtio_mmio_init;
		vdev->ops->exit			= virtio_mmio_exit;
		vdev->ops->reset		= virtio_mmio_reset;
		r = vdev->ops->init(kvm, dev, vdev, device_id, subsys_id, class);
		break;
	default:
		r = -1;
	};

	return r;
}

int virtio_compat_add_message(const char *device, const char *config)
{
	int len = 1024;
	int compat_id;
	char *title;
	char *desc;

	title = malloc(len);
	if (!title)
		return -ENOMEM;

	desc = malloc(len);
	if (!desc) {
		free(title);
		return -ENOMEM;
	}

	snprintf(title, len, "%s device was not detected.", device);
	snprintf(desc,  len, "While you have requested a %s device, "
			     "the guest kernel did not initialize it.\n"
			     "\tPlease make sure that the guest kernel was "
			     "compiled with %s=y enabled in .config.",
			     device, config);

	compat_id = compat__add_message(title, desc);

	free(desc);
	free(title);

	return compat_id;
}
