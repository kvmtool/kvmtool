#include "kvm/virtio-pci.h"

#include "kvm/ioport.h"
#include "kvm/kvm.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/irq.h"
#include "kvm/virtio.h"
#include "kvm/ioeventfd.h"

#include <sys/ioctl.h>
#include <linux/virtio_pci.h>
#include <linux/byteorder.h>
#include <string.h>

static void virtio_pci__ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_pci_ioevent_param *ioeventfd = param;
	struct virtio_pci *vpci = ioeventfd->vdev->virtio;

	ioeventfd->vdev->ops->notify_vq(kvm, vpci->dev, ioeventfd->vq);
}

static int virtio_pci__init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct ioevent ioevent;
	struct virtio_pci *vpci = vdev->virtio;
	int i, r, flags = IOEVENTFD_FLAG_PIO;
	int fds[2];

	vpci->ioeventfds[vq] = (struct virtio_pci_ioevent_param) {
		.vdev		= vdev,
		.vq		= vq,
	};

	ioevent = (struct ioevent) {
		.fn		= virtio_pci__ioevent_callback,
		.fn_ptr		= &vpci->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
	};

	/*
	 * Vhost will poll the eventfd in host kernel side, otherwise we
	 * need to poll in userspace.
	 */
	if (!vdev->use_vhost)
		flags |= IOEVENTFD_FLAG_USER_POLL;

	/* ioport */
	ioevent.io_addr	= vpci->port_addr + VIRTIO_PCI_QUEUE_NOTIFY;
	ioevent.io_len	= sizeof(u16);
	ioevent.fd	= fds[0] = eventfd(0, 0);
	r = ioeventfd__add_event(&ioevent, flags);
	if (r)
		return r;

	/* mmio */
	ioevent.io_addr	= vpci->mmio_addr + VIRTIO_PCI_QUEUE_NOTIFY;
	ioevent.io_len	= sizeof(u32);
	ioevent.fd	= fds[1] = eventfd(0, 0);
	r = ioeventfd__add_event(&ioevent, flags);
	if (r)
		goto free_ioport_evt;

	if (vdev->ops->notify_vq_eventfd)
		for (i = 0; i < 2; ++i)
			vdev->ops->notify_vq_eventfd(kvm, vpci->dev, vq,
						     fds[i]);
	return 0;

free_ioport_evt:
	ioeventfd__del_event(vpci->port_addr + VIRTIO_PCI_QUEUE_NOTIFY, vq);
	return r;
}

static inline bool virtio_pci__msix_enabled(struct virtio_pci *vpci)
{
	return vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_ENABLE);
}

static bool virtio_pci__specific_io_in(struct kvm *kvm, struct virtio_device *vdev, u16 port,
					void *data, int size, int offset)
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
		u8 cfg;

		cfg = vdev->ops->get_config(kvm, vpci->dev)[config_offset];
		ioport__write8(data, cfg);
		return true;
	}

	return false;
}

static bool virtio_pci__io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset;
	bool ret = true;
	struct virtio_device *vdev;
	struct virtio_pci *vpci;
	u32 val;

	vdev = ioport->priv;
	vpci = vdev->virtio;
	offset = port - vpci->port_addr;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		val = vdev->ops->get_host_features(kvm, vpci->dev);
		ioport__write32(data, val);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		val = vdev->ops->get_pfn_vq(kvm, vpci->dev, vpci->queue_selector);
		ioport__write32(data, val);
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
		kvm__irq_line(kvm, vpci->pci_hdr.irq_line, VIRTIO_IRQ_LOW);
		vpci->isr = VIRTIO_IRQ_LOW;
		break;
	default:
		ret = virtio_pci__specific_io_in(kvm, vdev, port, data, size, offset);
		break;
	};

	return ret;
}

static bool virtio_pci__specific_io_out(struct kvm *kvm, struct virtio_device *vdev, u16 port,
					void *data, int size, int offset)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 config_offset, gsi, vec;
	int type = virtio__get_dev_specific_field(offset - 20, virtio_pci__msix_enabled(vpci),
							&config_offset);
	if (type == VIRTIO_PCI_O_MSIX) {
		switch (offset) {
		case VIRTIO_MSI_CONFIG_VECTOR:
			vec = vpci->config_vector = ioport__read16(data);
			if (vec == VIRTIO_MSI_NO_VECTOR)
				break;

			gsi = irq__add_msix_route(kvm, &vpci->msix_table[vec].msg);

			vpci->config_gsi = gsi;
			break;
		case VIRTIO_MSI_QUEUE_VECTOR:
			vec = vpci->vq_vector[vpci->queue_selector] = ioport__read16(data);

			if (vec == VIRTIO_MSI_NO_VECTOR)
				break;

			gsi = irq__add_msix_route(kvm, &vpci->msix_table[vec].msg);
			vpci->gsis[vpci->queue_selector] = gsi;
			if (vdev->ops->notify_vq_gsi)
				vdev->ops->notify_vq_gsi(kvm, vpci->dev,
							vpci->queue_selector, gsi);
			break;
		};

		return true;
	} else if (type == VIRTIO_PCI_O_CONFIG) {
		vdev->ops->get_config(kvm, vpci->dev)[config_offset] = *(u8 *)data;

		return true;
	}

	return false;
}

static bool virtio_pci__io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset;
	bool ret = true;
	struct virtio_device *vdev;
	struct virtio_pci *vpci;
	u32 val;

	vdev = ioport->priv;
	vpci = vdev->virtio;
	offset = port - vpci->port_addr;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		val = ioport__read32(data);
		vdev->ops->set_guest_features(kvm, vpci->dev, val);
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		val = ioport__read32(data);
		virtio_pci__init_ioeventfd(kvm, vdev, vpci->queue_selector);
		vdev->ops->init_vq(kvm, vpci->dev, vpci->queue_selector,
				   1 << VIRTIO_PCI_QUEUE_ADDR_SHIFT,
				   VIRTIO_PCI_VRING_ALIGN, val);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
		vpci->queue_selector = ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY:
		val = ioport__read16(data);
		vdev->ops->notify_vq(kvm, vpci->dev, val);
		break;
	case VIRTIO_PCI_STATUS:
		vpci->status = ioport__read8(data);
		if (vdev->ops->notify_status)
			vdev->ops->notify_status(kvm, vpci->dev, vpci->status);
		break;
	default:
		ret = virtio_pci__specific_io_out(kvm, vdev, port, data, size, offset);
		break;
	};

	return ret;
}

static struct ioport_operations virtio_pci__io_ops = {
	.io_in	= virtio_pci__io_in,
	.io_out	= virtio_pci__io_out,
};

static void virtio_pci__msix_mmio_callback(u64 addr, u8 *data, u32 len,
					   u8 is_write, void *ptr)
{
	struct virtio_pci *vpci = ptr;
	void *table;
	u32 offset;

	if (addr > vpci->msix_io_block + PCI_IO_SIZE) {
		table	= &vpci->msix_pba;
		offset	= vpci->msix_io_block + PCI_IO_SIZE;
	} else {
		table	= &vpci->msix_table;
		offset	= vpci->msix_io_block;
	}

	if (is_write)
		memcpy(table + addr - offset, data, len);
	else
		memcpy(data, table + addr - offset, len);
}

static void virtio_pci__signal_msi(struct kvm *kvm, struct virtio_pci *vpci, int vec)
{
	struct kvm_msi msi = {
		.address_lo = vpci->msix_table[vec].msg.address_lo,
		.address_hi = vpci->msix_table[vec].msg.address_hi,
		.data = vpci->msix_table[vec].msg.data,
	};

	ioctl(kvm->vm_fd, KVM_SIGNAL_MSI, &msi);
}

int virtio_pci__signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_pci *vpci = vdev->virtio;
	int tbl = vpci->vq_vector[vq];

	if (virtio_pci__msix_enabled(vpci) && tbl != VIRTIO_MSI_NO_VECTOR) {
		if (vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_MASKALL) ||
		    vpci->msix_table[tbl].ctrl & cpu_to_le16(PCI_MSIX_ENTRY_CTRL_MASKBIT)) {

			vpci->msix_pba |= 1 << tbl;
			return 0;
		}

		if (vpci->features & VIRTIO_PCI_F_SIGNAL_MSI)
			virtio_pci__signal_msi(kvm, vpci, vpci->vq_vector[vq]);
		else
			kvm__irq_trigger(kvm, vpci->gsis[vq]);
	} else {
		vpci->isr = VIRTIO_IRQ_HIGH;
		kvm__irq_trigger(kvm, vpci->pci_hdr.irq_line);
	}
	return 0;
}

int virtio_pci__signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_pci *vpci = vdev->virtio;
	int tbl = vpci->config_vector;

	if (virtio_pci__msix_enabled(vpci) && tbl != VIRTIO_MSI_NO_VECTOR) {
		if (vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_MASKALL) ||
		    vpci->msix_table[tbl].ctrl & cpu_to_le16(PCI_MSIX_ENTRY_CTRL_MASKBIT)) {

			vpci->msix_pba |= 1 << tbl;
			return 0;
		}

		if (vpci->features & VIRTIO_PCI_F_SIGNAL_MSI)
			virtio_pci__signal_msi(kvm, vpci, tbl);
		else
			kvm__irq_trigger(kvm, vpci->config_gsi);
	} else {
		vpci->isr = VIRTIO_PCI_ISR_CONFIG;
		kvm__irq_trigger(kvm, vpci->pci_hdr.irq_line);
	}

	return 0;
}

static void virtio_pci__io_mmio_callback(u64 addr, u8 *data, u32 len,
					 u8 is_write, void *ptr)
{
	struct virtio_pci *vpci = ptr;
	int direction = is_write ? KVM_EXIT_IO_OUT : KVM_EXIT_IO_IN;
	u16 port = vpci->port_addr + (addr & (IOPORT_SIZE - 1));

	kvm__emulate_io(vpci->kvm, port, data, direction, len, 1);
}

int virtio_pci__init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class)
{
	struct virtio_pci *vpci = vdev->virtio;
	int r;

	vpci->kvm = kvm;
	vpci->dev = dev;

	r = ioport__register(kvm, IOPORT_EMPTY, &virtio_pci__io_ops, IOPORT_SIZE, vdev);
	if (r < 0)
		return r;
	vpci->port_addr = (u16)r;

	vpci->mmio_addr = pci_get_io_space_block(IOPORT_SIZE);
	r = kvm__register_mmio(kvm, vpci->mmio_addr, IOPORT_SIZE, false,
			       virtio_pci__io_mmio_callback, vpci);
	if (r < 0)
		goto free_ioport;

	vpci->msix_io_block = pci_get_io_space_block(PCI_IO_SIZE * 2);
	r = kvm__register_mmio(kvm, vpci->msix_io_block, PCI_IO_SIZE * 2, false,
			       virtio_pci__msix_mmio_callback, vpci);
	if (r < 0)
		goto free_mmio;

	vpci->pci_hdr = (struct pci_device_header) {
		.vendor_id		= cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET),
		.device_id		= cpu_to_le16(device_id),
		.command		= PCI_COMMAND_IO | PCI_COMMAND_MEMORY,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= 0,
		.class[0]		= class & 0xff,
		.class[1]		= (class >> 8) & 0xff,
		.class[2]		= (class >> 16) & 0xff,
		.subsys_vendor_id	= cpu_to_le16(PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET),
		.subsys_id		= cpu_to_le16(subsys_id),
		.bar[0]			= cpu_to_le32(vpci->mmio_addr
							| PCI_BASE_ADDRESS_SPACE_MEMORY),
		.bar[1]			= cpu_to_le32(vpci->port_addr
							| PCI_BASE_ADDRESS_SPACE_IO),
		.bar[2]			= cpu_to_le32(vpci->msix_io_block
							| PCI_BASE_ADDRESS_SPACE_MEMORY),
		.status			= cpu_to_le16(PCI_STATUS_CAP_LIST),
		.capabilities		= (void *)&vpci->pci_hdr.msix - (void *)&vpci->pci_hdr,
		.bar_size[0]		= IOPORT_SIZE,
		.bar_size[1]		= IOPORT_SIZE,
		.bar_size[2]		= PCI_IO_SIZE * 2,
	};

	vpci->dev_hdr = (struct device_header) {
		.bus_type		= DEVICE_BUS_PCI,
		.data			= &vpci->pci_hdr,
	};

	vpci->pci_hdr.msix.cap = PCI_CAP_ID_MSIX;
	vpci->pci_hdr.msix.next = 0;
	/*
	 * We at most have VIRTIO_PCI_MAX_VQ entries for virt queue,
	 * VIRTIO_PCI_MAX_CONFIG entries for config.
	 *
	 * To quote the PCI spec:
	 *
	 * System software reads this field to determine the
	 * MSI-X Table Size N, which is encoded as N-1.
	 * For example, a returned value of "00000000011"
	 * indicates a table size of 4.
	 */
	vpci->pci_hdr.msix.ctrl = cpu_to_le16(VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG - 1);

	/* Both table and PBA are mapped to the same BAR (2) */
	vpci->pci_hdr.msix.table_offset = cpu_to_le32(2);
	vpci->pci_hdr.msix.pba_offset = cpu_to_le32(2 | PCI_IO_SIZE);
	vpci->config_vector = 0;

	if (kvm__supports_extension(kvm, KVM_CAP_SIGNAL_MSI))
		vpci->features |= VIRTIO_PCI_F_SIGNAL_MSI;

	r = device__register(&vpci->dev_hdr);
	if (r < 0)
		goto free_msix_mmio;

	return 0;

free_msix_mmio:
	kvm__deregister_mmio(kvm, vpci->msix_io_block);
free_mmio:
	kvm__deregister_mmio(kvm, vpci->mmio_addr);
free_ioport:
	ioport__unregister(kvm, vpci->port_addr);
	return r;
}

int virtio_pci__exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_pci *vpci = vdev->virtio;
	int i;

	kvm__deregister_mmio(kvm, vpci->mmio_addr);
	kvm__deregister_mmio(kvm, vpci->msix_io_block);
	ioport__unregister(kvm, vpci->port_addr);

	for (i = 0; i < VIRTIO_PCI_MAX_VQ; i++) {
		ioeventfd__del_event(vpci->port_addr + VIRTIO_PCI_QUEUE_NOTIFY, i);
		ioeventfd__del_event(vpci->mmio_addr + VIRTIO_PCI_QUEUE_NOTIFY, i);
	}

	return 0;
}
