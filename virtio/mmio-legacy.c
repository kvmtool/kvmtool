#include "kvm/ioport.h"
#include "kvm/virtio.h"
#include "kvm/virtio-mmio.h"

#include <linux/virtio_mmio.h>

#define vmmio_selected_vq(vdev, vmmio) \
	(vdev)->ops->get_vq((vmmio)->kvm, (vmmio)->dev, (vmmio)->hdr.queue_sel)

static void virtio_mmio_config_in(struct kvm_cpu *vcpu,
				  u64 addr, void *data, u32 len,
				  struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct virt_queue *vq;
	u32 val = 0;

	switch (addr) {
	case VIRTIO_MMIO_MAGIC_VALUE:
	case VIRTIO_MMIO_VERSION:
	case VIRTIO_MMIO_DEVICE_ID:
	case VIRTIO_MMIO_VENDOR_ID:
	case VIRTIO_MMIO_STATUS:
	case VIRTIO_MMIO_INTERRUPT_STATUS:
		ioport__write32(data, *(u32 *)(((void *)&vmmio->hdr) + addr));
		break;
	case VIRTIO_MMIO_DEVICE_FEATURES:
		if (vmmio->hdr.host_features_sel == 0)
			val = vdev->ops->get_host_features(vmmio->kvm,
							   vmmio->dev);
		ioport__write32(data, val);
		break;
	case VIRTIO_MMIO_QUEUE_PFN:
		vq = vmmio_selected_vq(vdev, vmmio);
		ioport__write32(data, vq->vring_addr.pfn);
		break;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		val = vdev->ops->get_size_vq(vmmio->kvm, vmmio->dev,
					     vmmio->hdr.queue_sel);
		ioport__write32(data, val);
		break;
	default:
		break;
	}
}

static void virtio_mmio_config_out(struct kvm_cpu *vcpu,
				   u64 addr, void *data, u32 len,
				   struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct kvm *kvm = vmmio->kvm;
	unsigned int vq_count = vdev->ops->get_vq_count(kvm, vmmio->dev);
	struct virt_queue *vq;
	u32 val = 0;

	switch (addr) {
	case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
	case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
		val = ioport__read32(data);
		*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		val = ioport__read32(data);
		if (val >= vq_count) {
			WARN_ONCE(1, "QUEUE_SEL value (%u) is larger than VQ count (%u)\n",
				val, vq_count);
			break;
		}
		*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		break;
	case VIRTIO_MMIO_STATUS:
		vmmio->hdr.status = ioport__read32(data);
		if (!vmmio->hdr.status) /* Sample endianness on reset */
			vdev->endian = kvm_cpu__get_endianness(vcpu);
		virtio_notify_status(kvm, vdev, vmmio->dev, vmmio->hdr.status);
		break;
	case VIRTIO_MMIO_DRIVER_FEATURES:
		if (vmmio->hdr.guest_features_sel == 0) {
			val = ioport__read32(data);
			virtio_set_guest_features(vmmio->kvm, vdev,
						  vmmio->dev, val);
		}
		break;
	case VIRTIO_MMIO_GUEST_PAGE_SIZE:
		val = ioport__read32(data);
		vmmio->hdr.guest_page_size = val;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		val = ioport__read32(data);
		vmmio->hdr.queue_num = val;
		vdev->ops->set_size_vq(vmmio->kvm, vmmio->dev,
				       vmmio->hdr.queue_sel, val);
		break;
	case VIRTIO_MMIO_QUEUE_ALIGN:
		val = ioport__read32(data);
		vmmio->hdr.queue_align = val;
		break;
	case VIRTIO_MMIO_QUEUE_PFN:
		val = ioport__read32(data);
		if (val) {
			vq = vmmio_selected_vq(vdev, vmmio);
			vq->vring_addr = (struct vring_addr) {
				.legacy	= true,
				.pfn	= val,
				.align	= vmmio->hdr.queue_align,
				.pgsize	= vmmio->hdr.guest_page_size,
			};
			virtio_mmio_init_vq(kvm, vdev, vmmio->hdr.queue_sel);
		} else {
			virtio_mmio_exit_vq(kvm, vdev, vmmio->hdr.queue_sel);
		}
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		val = ioport__read32(data);
		if (val >= vq_count) {
			WARN_ONCE(1, "QUEUE_NOTIFY value (%u) is larger than VQ count (%u)\n",
				val, vq_count);
			break;
		}
		vdev->ops->notify_vq(vmmio->kvm, vmmio->dev, val);
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		val = ioport__read32(data);
		vmmio->hdr.interrupt_state &= ~val;
		break;
	default:
		break;
	};
}

void virtio_mmio_legacy_callback(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				 u32 len, u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 offset = addr - vmmio->addr;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;
		virtio_access_config(vmmio->kvm, vdev, vmmio->dev, offset, data,
				     len, is_write);
		return;
	}

	if (is_write)
		virtio_mmio_config_out(vcpu, offset, data, len, ptr);
	else
		virtio_mmio_config_in(vcpu, offset, data, len, ptr);
}
