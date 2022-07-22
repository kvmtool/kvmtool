#include "kvm/virtio-pci.h"

#include "kvm/ioport.h"
#include "kvm/virtio.h"
#include "kvm/virtio-pci-dev.h"

#include <linux/virtio_config.h>

#define VPCI_CFG_COMMON_SIZE	sizeof(struct virtio_pci_common_cfg)
#define VPCI_CFG_COMMON_START	0
#define VPCI_CFG_COMMON_END	(VPCI_CFG_COMMON_SIZE - 1)
/*
 * Use a naturally aligned 4-byte doorbell, in case we ever want to
 * implement VIRTIO_F_NOTIFICATION_DATA
 */
#define VPCI_CFG_NOTIFY_SIZE	4
#define VPCI_CFG_NOTIFY_START	(VPCI_CFG_COMMON_END + 1)
#define VPCI_CFG_NOTIFY_END	(VPCI_CFG_COMMON_END + VPCI_CFG_NOTIFY_SIZE)
#define VPCI_CFG_ISR_SIZE	4
#define VPCI_CFG_ISR_START	(VPCI_CFG_NOTIFY_END + 1)
#define VPCI_CFG_ISR_END	(VPCI_CFG_NOTIFY_END + VPCI_CFG_ISR_SIZE)
/*
 * We're at 64 bytes. Use the remaining 192 bytes in PCI_IO_SIZE for the
 * device-specific config space. It's sufficient for the devices we
 * currently implement (virtio_blk_config is 60 bytes) and, I think, all
 * existing virtio 1.2 devices.
 */
#define VPCI_CFG_DEV_START	(VPCI_CFG_ISR_END + 1)
#define VPCI_CFG_DEV_END	((PCI_IO_SIZE) - 1)
#define VPCI_CFG_DEV_SIZE	(VPCI_CFG_DEV_END - VPCI_CFG_DEV_START + 1)

#define vpci_selected_vq(vpci) \
	vdev->ops->get_vq((vpci)->kvm, (vpci)->dev, (vpci)->queue_selector)

typedef bool (*access_handler_t)(struct virtio_device *, unsigned long, void *, int);

static bool virtio_pci__common_write(struct virtio_device *vdev,
				     unsigned long offset, void *data, int size)
{
	u64 features;
	u32 val, gsi, vec;
	struct virtio_pci *vpci = vdev->virtio;

	switch (offset - VPCI_CFG_COMMON_START) {
	case VIRTIO_PCI_COMMON_DFSELECT:
		vpci->device_features_sel = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_GFSELECT:
		vpci->driver_features_sel = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_GF:
		val = ioport__read32(data);
		if (vpci->driver_features_sel > 1)
			break;

		features = (u64)val << (32 * vpci->driver_features_sel);
		virtio_set_guest_features(vpci->kvm, vdev, vpci->dev, features);
		break;
	case VIRTIO_PCI_COMMON_MSIX:
		vec = vpci->config_vector = ioport__read16(data);
		gsi = virtio_pci__add_msix_route(vpci, vec);
		if (gsi < 0)
			break;

		vpci->config_gsi = gsi;
		break;
	case VIRTIO_PCI_COMMON_STATUS:
		vpci->status = ioport__read8(data);
		virtio_notify_status(vpci->kvm, vdev, vpci->dev, vpci->status);
		break;
	case VIRTIO_PCI_COMMON_Q_SELECT:
		val = ioport__read16(data);
		if (val >= (u32)vdev->ops->get_vq_count(vpci->kvm, vpci->dev))
			pr_warning("invalid vq number %u", val);
		else
			vpci->queue_selector = val;
		break;
	case VIRTIO_PCI_COMMON_Q_SIZE:
		vdev->ops->set_size_vq(vpci->kvm, vpci->dev,
				       vpci->queue_selector,
				       ioport__read16(data));
		break;
	case VIRTIO_PCI_COMMON_Q_MSIX:
		vec = vpci->vq_vector[vpci->queue_selector] = ioport__read16(data);

		gsi = virtio_pci__add_msix_route(vpci, vec);
		if (gsi < 0)
			break;

		vpci->gsis[vpci->queue_selector] = gsi;
		if (vdev->ops->notify_vq_gsi)
			vdev->ops->notify_vq_gsi(vpci->kvm, vpci->dev,
						 vpci->queue_selector, gsi);
		break;
	case VIRTIO_PCI_COMMON_Q_ENABLE:
		val = ioport__read16(data);
		if (val)
			virtio_pci_init_vq(vpci->kvm, vdev, vpci->queue_selector);
		else
			virtio_pci_exit_vq(vpci->kvm, vdev, vpci->queue_selector);
		break;
	case VIRTIO_PCI_COMMON_Q_DESCLO:
		vpci_selected_vq(vpci)->vring_addr.desc_lo = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_Q_DESCHI:
		vpci_selected_vq(vpci)->vring_addr.desc_hi = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILLO:
		vpci_selected_vq(vpci)->vring_addr.avail_lo = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILHI:
		vpci_selected_vq(vpci)->vring_addr.avail_hi = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_Q_USEDLO:
		vpci_selected_vq(vpci)->vring_addr.used_lo = ioport__read32(data);
		break;
	case VIRTIO_PCI_COMMON_Q_USEDHI:
		vpci_selected_vq(vpci)->vring_addr.used_hi = ioport__read32(data);
		break;
	}

	return true;
}

static bool virtio_pci__notify_write(struct virtio_device *vdev,
				     unsigned long offset, void *data, int size)
{
	u16 vq = ioport__read16(data);
	struct virtio_pci *vpci = vdev->virtio;

	vdev->ops->notify_vq(vpci->kvm, vpci->dev, vq);

	return true;
}

static bool virtio_pci__config_write(struct virtio_device *vdev,
				     unsigned long offset, void *data, int size)
{
	struct virtio_pci *vpci = vdev->virtio;

	return virtio_access_config(vpci->kvm, vdev, vpci->dev,
				    offset - VPCI_CFG_DEV_START, data, size,
				    true);
}

static bool virtio_pci__common_read(struct virtio_device *vdev,
				    unsigned long offset, void *data, int size)
{
	u32 val;
	struct virtio_pci *vpci = vdev->virtio;
	u64 features = 1ULL << VIRTIO_F_VERSION_1;

	switch (offset - VPCI_CFG_COMMON_START) {
	case VIRTIO_PCI_COMMON_DFSELECT:
		val = vpci->device_features_sel;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_DF:
		if (vpci->device_features_sel > 1)
			break;
		features |= vdev->ops->get_host_features(vpci->kvm, vpci->dev);
		val = features >> (32 * vpci->device_features_sel);
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_GFSELECT:
		val = vpci->driver_features_sel;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_MSIX:
		val = vpci->config_vector;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_NUMQ:
		val = vdev->ops->get_vq_count(vpci->kvm, vpci->dev);
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_STATUS:
		ioport__write8(data, vpci->status);
		break;
	case VIRTIO_PCI_COMMON_CFGGENERATION:
		/*
		 * The config generation changes when the device updates a
		 * config field larger than 32 bits, that the driver may read
		 * using multiple accesses. Since kvmtool doesn't use any
		 * mutable config field larger than 32 bits, the generation is
		 * constant.
		 */
		ioport__write8(data, 0);
		break;
	case VIRTIO_PCI_COMMON_Q_SELECT:
		ioport__write16(data, vpci->queue_selector);
		break;
	case VIRTIO_PCI_COMMON_Q_SIZE:
		val = vdev->ops->get_size_vq(vpci->kvm, vpci->dev,
					     vpci->queue_selector);
		ioport__write16(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_MSIX:
		val = vpci->vq_vector[vpci->queue_selector];
		ioport__write16(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_ENABLE:
		val = vpci_selected_vq(vpci)->enabled;
		ioport__write16(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_NOFF:
		val = vpci->queue_selector;
		ioport__write16(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_DESCLO:
		val = vpci_selected_vq(vpci)->vring_addr.desc_lo;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_DESCHI:
		val = vpci_selected_vq(vpci)->vring_addr.desc_hi;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILLO:
		val = vpci_selected_vq(vpci)->vring_addr.avail_lo;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_AVAILHI:
		val = vpci_selected_vq(vpci)->vring_addr.avail_hi;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_USEDLO:
		val = vpci_selected_vq(vpci)->vring_addr.used_lo;
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_COMMON_Q_USEDHI:
		val = vpci_selected_vq(vpci)->vring_addr.used_hi;
		ioport__write32(data, val);
		break;
	};

	return true;
}

static bool virtio_pci__isr_read(struct virtio_device *vdev,
				 unsigned long offset, void *data, int size)
{
	struct virtio_pci *vpci = vdev->virtio;

	if (WARN_ON(offset - VPCI_CFG_ISR_START != 0))
		return false;

	ioport__write8(data, vpci->isr);
	kvm__irq_line(vpci->kvm, vpci->legacy_irq_line, VIRTIO_IRQ_LOW);
	vpci->isr = 0;

	return 0;
}

static bool virtio_pci__config_read(struct virtio_device *vdev,
				    unsigned long offset, void *data, int size)
{
	struct virtio_pci *vpci = vdev->virtio;

	return virtio_access_config(vpci->kvm, vdev, vpci->dev,
				    offset - VPCI_CFG_DEV_START, data, size,
				    false);
}

static bool virtio_pci_access(struct kvm_cpu *vcpu, struct virtio_device *vdev,
			      unsigned long offset, void *data, int size,
			      bool write)
{
	access_handler_t handler = NULL;

	switch (offset) {
	case VPCI_CFG_COMMON_START...VPCI_CFG_COMMON_END:
		if (write)
			handler = virtio_pci__common_write;
		else
			handler = virtio_pci__common_read;
		break;
	case VPCI_CFG_NOTIFY_START...VPCI_CFG_NOTIFY_END:
		if (write)
			handler = virtio_pci__notify_write;
		break;
	case VPCI_CFG_ISR_START...VPCI_CFG_ISR_END:
		if (!write)
			handler = virtio_pci__isr_read;
		break;
	case VPCI_CFG_DEV_START...VPCI_CFG_DEV_END:
		if (write)
			handler = virtio_pci__config_write;
		else
			handler = virtio_pci__config_read;
		break;
	}

	if (!handler)
		return false;

	return handler(vdev, offset, data, size);
}

void virtio_pci_modern__io_mmio_callback(struct kvm_cpu *vcpu, u64 addr,
					 u8 *data, u32 len, u8 is_write,
					 void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr = virtio_pci__mmio_addr(vpci);

	virtio_pci_access(vcpu, vdev, addr - mmio_addr, data, len, is_write);
}

int virtio_pci_modern_init(struct virtio_device *vdev)
{
	int subsys_id;
	struct virtio_pci *vpci = vdev->virtio;
	struct pci_device_header *hdr = &vpci->pci_hdr;

	subsys_id = le16_to_cpu(hdr->subsys_id);

	hdr->device_id = cpu_to_le16(PCI_DEVICE_ID_VIRTIO_BASE + subsys_id);
	hdr->subsys_id = cpu_to_le16(PCI_SUBSYS_ID_VIRTIO_BASE + subsys_id);

	vpci->doorbell_offset = VPCI_CFG_NOTIFY_START;
	vdev->endian = VIRTIO_ENDIAN_LE;

	hdr->msix.next = PCI_CAP_OFF(hdr, virtio);

	hdr->virtio.common = (struct virtio_pci_cap) {
		.cap_vndr		= PCI_CAP_ID_VNDR,
		.cap_next		= PCI_CAP_OFF(hdr, virtio.notify),
		.cap_len		= sizeof(hdr->virtio.common),
		.cfg_type		= VIRTIO_PCI_CAP_COMMON_CFG,
		.bar			= 1,
		.offset			= cpu_to_le32(VPCI_CFG_COMMON_START),
		.length			= cpu_to_le32(VPCI_CFG_COMMON_SIZE),
	};
	BUILD_BUG_ON(VPCI_CFG_COMMON_START & 0x3);

	hdr->virtio.notify = (struct virtio_pci_notify_cap) {
		.cap.cap_vndr		= PCI_CAP_ID_VNDR,
		.cap.cap_next		= PCI_CAP_OFF(hdr, virtio.isr),
		.cap.cap_len		= sizeof(hdr->virtio.notify),
		.cap.cfg_type		= VIRTIO_PCI_CAP_NOTIFY_CFG,
		.cap.bar		= 1,
		.cap.offset		= cpu_to_le32(VPCI_CFG_NOTIFY_START),
		.cap.length		= cpu_to_le32(VPCI_CFG_NOTIFY_SIZE),
		/*
		 * Notify multiplier is 0, meaning that notifications are all on
		 * the same register
		 */
	};
	BUILD_BUG_ON(VPCI_CFG_NOTIFY_START & 0x3);

	hdr->virtio.isr = (struct virtio_pci_cap) {
		.cap_vndr		= PCI_CAP_ID_VNDR,
		.cap_next		= PCI_CAP_OFF(hdr, virtio.device),
		.cap_len		= sizeof(hdr->virtio.isr),
		.cfg_type		= VIRTIO_PCI_CAP_ISR_CFG,
		.bar			= 1,
		.offset			= cpu_to_le32(VPCI_CFG_ISR_START),
		.length			= cpu_to_le32(VPCI_CFG_ISR_SIZE),
	};

	hdr->virtio.device = (struct virtio_pci_cap) {
		.cap_vndr		= PCI_CAP_ID_VNDR,
		.cap_next		= PCI_CAP_OFF(hdr, virtio.pci),
		.cap_len		= sizeof(hdr->virtio.device),
		.cfg_type		= VIRTIO_PCI_CAP_DEVICE_CFG,
		.bar			= 1,
		.offset			= cpu_to_le32(VPCI_CFG_DEV_START),
		.length			= cpu_to_le32(VPCI_CFG_DEV_SIZE),
	};
	BUILD_BUG_ON(VPCI_CFG_DEV_START & 0x3);

	/*
	 * TODO: implement this weird proxy capability (it is a "MUST" in the
	 * spec, but I don't know if anyone actually uses it).
	 * It doesn't use any BAR space. Instead the driver writes .cap.offset
	 * and .cap.length to access a register in a BAR.
	 */
	hdr->virtio.pci = (struct virtio_pci_cfg_cap) {
		.cap.cap_vndr		= PCI_CAP_ID_VNDR,
		.cap.cap_next		= 0,
		.cap.cap_len		= sizeof(hdr->virtio.pci),
		.cap.cfg_type		= VIRTIO_PCI_CAP_PCI_CFG,
	};

	return 0;
}
