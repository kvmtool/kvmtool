#include "kvm/virtio-pci.h"

#include "kvm/ioport.h"
#include "kvm/virtio.h"

static bool virtio_pci__specific_data_in(struct kvm *kvm, struct virtio_device *vdev,
					 void *data, u32 size, unsigned long offset)
{
	u32 config_offset;
	struct virtio_pci *vpci = vdev->virtio;
	int type = virtio__get_dev_specific_field(offset - 20,
							virtio_pci__msix_enabled(vpci),
							&config_offset);
	if (type == VIRTIO_PCI_O_MSIX) {
		switch (offset) {
		case VIRTIO_MSI_CONFIG_VECTOR:
			ioport__write16(data, vpci->config_vector);
			break;
		case VIRTIO_MSI_QUEUE_VECTOR:
			ioport__write16(data, vpci->vq_vector[vpci->queue_selector]);
			break;
		};

		return true;
	} else if (type == VIRTIO_PCI_O_CONFIG) {
		return virtio_access_config(kvm, vdev, vpci->dev, config_offset,
					    data, size, false);
	}

	return false;
}

static bool virtio_pci__data_in(struct kvm_cpu *vcpu, struct virtio_device *vdev,
				unsigned long offset, void *data, u32 size)
{
	bool ret = true;
	struct virtio_pci *vpci;
	struct virt_queue *vq;
	struct kvm *kvm;
	u32 val;

	kvm = vcpu->kvm;
	vpci = vdev->virtio;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		val = vdev->ops->get_host_features(kvm, vpci->dev);
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		vq = vdev->ops->get_vq(kvm, vpci->dev, vpci->queue_selector);
		ioport__write32(data, vq->vring_addr.pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		val = vdev->ops->get_size_vq(kvm, vpci->dev, vpci->queue_selector);
		ioport__write16(data, val);
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, vpci->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, vpci->isr);
		kvm__irq_line(kvm, vpci->legacy_irq_line, VIRTIO_IRQ_LOW);
		vpci->isr = VIRTIO_IRQ_LOW;
		break;
	default:
		ret = virtio_pci__specific_data_in(kvm, vdev, data, size, offset);
		break;
	};

	return ret;
}

static bool virtio_pci__specific_data_out(struct kvm *kvm, struct virtio_device *vdev,
					  void *data, u32 size, unsigned long offset)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 config_offset, vec;
	int gsi;
	int type = virtio__get_dev_specific_field(offset - 20, virtio_pci__msix_enabled(vpci),
							&config_offset);
	if (type == VIRTIO_PCI_O_MSIX) {
		switch (offset) {
		case VIRTIO_MSI_CONFIG_VECTOR:
			vec = vpci->config_vector = ioport__read16(data);

			gsi = virtio_pci__add_msix_route(vpci, vec);
			if (gsi < 0)
				break;

			vpci->config_gsi = gsi;
			break;
		case VIRTIO_MSI_QUEUE_VECTOR:
			vec = ioport__read16(data);
			vpci->vq_vector[vpci->queue_selector] = vec;

			gsi = virtio_pci__add_msix_route(vpci, vec);
			if (gsi < 0)
				break;

			vpci->gsis[vpci->queue_selector] = gsi;
			if (vdev->ops->notify_vq_gsi)
				vdev->ops->notify_vq_gsi(kvm, vpci->dev,
							 vpci->queue_selector,
							 gsi);
			break;
		};

		return true;
	} else if (type == VIRTIO_PCI_O_CONFIG) {
		return virtio_access_config(kvm, vdev, vpci->dev, config_offset,
					    data, size, true);
	}

	return false;
}

static bool virtio_pci__data_out(struct kvm_cpu *vcpu, struct virtio_device *vdev,
				 unsigned long offset, void *data, u32 size)
{
	bool ret = true;
	struct virtio_pci *vpci;
	struct virt_queue *vq;
	struct kvm *kvm;
	u32 val;
	unsigned int vq_count;

	kvm = vcpu->kvm;
	vpci = vdev->virtio;
	vq_count = vdev->ops->get_vq_count(kvm, vpci->dev);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		val = ioport__read32(data);
		virtio_set_guest_features(kvm, vdev, vpci->dev, val);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		val = ioport__read32(data);
		if (val) {
			vq = vdev->ops->get_vq(kvm, vpci->dev,
					       vpci->queue_selector);
			vq->vring_addr = (struct vring_addr) {
				.legacy	= true,
				.pfn	= val,
				.align	= VIRTIO_PCI_VRING_ALIGN,
				.pgsize	= 1 << VIRTIO_PCI_QUEUE_ADDR_SHIFT,
			};
			virtio_pci_init_vq(kvm, vdev, vpci->queue_selector);
		} else {
			virtio_pci_exit_vq(kvm, vdev, vpci->queue_selector);
		}
		break;
	case VIRTIO_PCI_QUEUE_SEL:
		val = ioport__read16(data);
		if (val >= vq_count) {
			WARN_ONCE(1, "QUEUE_SEL value (%u) is larger than VQ count (%u)\n",
				val, vq_count);
			return false;
		}
		vpci->queue_selector = val;
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY:
		val = ioport__read16(data);
		if (val >= vq_count) {
			WARN_ONCE(1, "QUEUE_SEL value (%u) is larger than VQ count (%u)\n",
				val, vq_count);
			return false;
		}
		vdev->ops->notify_vq(kvm, vpci->dev, val);
		break;
	case VIRTIO_PCI_STATUS:
		vpci->status = ioport__read8(data);
		if (!vpci->status) /* Sample endianness on reset */
			vdev->endian = kvm_cpu__get_endianness(vcpu);
		virtio_notify_status(kvm, vdev, vpci->dev, vpci->status);
		break;
	default:
		ret = virtio_pci__specific_data_out(kvm, vdev, data, size, offset);
		break;
	};

	return ret;
}

void virtio_pci_legacy__io_mmio_callback(struct kvm_cpu *vcpu, u64 addr,
					 u8 *data, u32 len, u8 is_write,
					 void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_pci *vpci = vdev->virtio;
	u32 ioport_addr = virtio_pci__port_addr(vpci);
	u32 base_addr;

	if (addr >= ioport_addr &&
	    addr < ioport_addr + pci__bar_size(&vpci->pci_hdr, 0))
		base_addr = ioport_addr;
	else
		base_addr = virtio_pci__mmio_addr(vpci);

	if (!is_write)
		virtio_pci__data_in(vcpu, vdev, addr - base_addr, data, len);
	else
		virtio_pci__data_out(vcpu, vdev, addr - base_addr, data, len);
}

