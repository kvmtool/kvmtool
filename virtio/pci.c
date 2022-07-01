#include "kvm/virtio-pci.h"

#include "kvm/ioport.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/irq.h"
#include "kvm/virtio.h"
#include "kvm/ioeventfd.h"
#include "kvm/util.h"

#include <sys/ioctl.h>
#include <linux/virtio_pci.h>
#include <assert.h>
#include <string.h>

int virtio_pci__add_msix_route(struct virtio_pci *vpci, u32 vec)
{
	int gsi;
	struct msi_msg *msg;

	if (vec == VIRTIO_MSI_NO_VECTOR)
		return -EINVAL;

	msg = &vpci->msix_table[vec].msg;
	gsi = irq__add_msix_route(vpci->kvm, msg, vpci->dev_hdr.dev_num << 3);
	/*
	 * We don't need IRQ routing if we can use
	 * MSI injection via the KVM_SIGNAL_MSI ioctl.
	 */
	if (gsi == -ENXIO && vpci->signal_msi)
		return gsi;

	if (gsi < 0)
		die("failed to configure MSIs");

	return gsi;
}

static void virtio_pci__del_msix_route(struct virtio_pci *vpci, u32 gsi)
{
	struct msi_msg msg = { 0 };

	irq__update_msix_route(vpci->kvm, gsi, &msg);
}

static void virtio_pci__ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_pci_ioevent_param *ioeventfd = param;
	struct virtio_pci *vpci = ioeventfd->vdev->virtio;

	ioeventfd->vdev->ops->notify_vq(kvm, vpci->dev, ioeventfd->vq);
}

int virtio_pci__init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev,
			       u32 vq)
{
	struct ioevent ioevent;
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr = virtio_pci__mmio_addr(vpci);
	u16 port_addr = virtio_pci__port_addr(vpci);
	off_t offset = vpci->doorbell_offset;
	int r, flags = 0;
	int pio_fd, mmio_fd;

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
	ioevent.io_addr	= port_addr + offset;
	ioevent.io_len	= sizeof(u16);
	ioevent.fd	= pio_fd = eventfd(0, 0);
	r = ioeventfd__add_event(&ioevent, flags | IOEVENTFD_FLAG_PIO);
	if (r)
		return r;

	/* mmio */
	ioevent.io_addr	= mmio_addr + offset;
	ioevent.io_len	= sizeof(u16);
	ioevent.fd	= mmio_fd = eventfd(0, 0);
	r = ioeventfd__add_event(&ioevent, flags);
	if (r)
		goto free_ioport_evt;

	if (vdev->ops->notify_vq_eventfd)
		vdev->ops->notify_vq_eventfd(kvm, vpci->dev, vq,
					     vdev->legacy ? pio_fd : mmio_fd);
	return 0;

free_ioport_evt:
	ioeventfd__del_event(port_addr + offset, vq);
	return r;
}

int virtio_pci_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	int ret;
	struct virtio_pci *vpci = vdev->virtio;

	ret = virtio_pci__init_ioeventfd(kvm, vdev, vq);
	if (ret) {
		pr_err("couldn't add ioeventfd for vq %d: %d", vq, ret);
		return ret;
	}
	return vdev->ops->init_vq(kvm, vpci->dev, vq);
}

void virtio_pci_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr = virtio_pci__mmio_addr(vpci);
	u16 port_addr = virtio_pci__port_addr(vpci);
	off_t offset = vpci->doorbell_offset;

	virtio_pci__del_msix_route(vpci, vpci->gsis[vq]);
	vpci->gsis[vq] = 0;
	vpci->vq_vector[vq] = VIRTIO_MSI_NO_VECTOR;
	ioeventfd__del_event(mmio_addr + offset, vq);
	ioeventfd__del_event(port_addr + offset, vq);
	virtio_exit_vq(kvm, vdev, vpci->dev, vq);
}

static void update_msix_map(struct virtio_pci *vpci,
			    struct msix_table *msix_entry, u32 vecnum)
{
	u32 gsi, i;

	/* Find the GSI number used for that vector */
	if (vecnum == vpci->config_vector) {
		gsi = vpci->config_gsi;
	} else {
		for (i = 0; i < VIRTIO_PCI_MAX_VQ; i++)
			if (vpci->vq_vector[i] == vecnum)
				break;
		if (i == VIRTIO_PCI_MAX_VQ)
			return;
		gsi = vpci->gsis[i];
	}

	if (gsi == 0)
		return;

	msix_entry = &msix_entry[vecnum];
	irq__update_msix_route(vpci->kvm, gsi, &msix_entry->msg);
}

static void virtio_pci__msix_mmio_callback(struct kvm_cpu *vcpu,
					   u64 addr, u8 *data, u32 len,
					   u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_pci *vpci = vdev->virtio;
	struct msix_table *table;
	u32 msix_io_addr = virtio_pci__msix_io_addr(vpci);
	u32 pba_offset;
	int vecnum;
	size_t offset;

	BUILD_BUG_ON(VIRTIO_NR_MSIX > (sizeof(vpci->msix_pba) * 8));

	pba_offset = vpci->pci_hdr.msix.pba_offset & ~PCI_MSIX_TABLE_BIR;
	if (addr >= msix_io_addr + pba_offset) {
		/* Read access to PBA */
		if (is_write)
			return;
		offset = addr - (msix_io_addr + pba_offset);
		if ((offset + len) > sizeof (vpci->msix_pba))
			return;
		memcpy(data, (void *)&vpci->msix_pba + offset, len);
		return;
	}

	table  = vpci->msix_table;
	offset = addr - msix_io_addr;

	vecnum = offset / sizeof(struct msix_table);
	offset = offset % sizeof(struct msix_table);

	if (!is_write) {
		memcpy(data, (void *)&table[vecnum] + offset, len);
		return;
	}

	memcpy((void *)&table[vecnum] + offset, data, len);

	/* Did we just update the address or payload? */
	if (offset < offsetof(struct msix_table, ctrl))
		update_msix_map(vpci, table, vecnum);
}

static void virtio_pci__signal_msi(struct kvm *kvm, struct virtio_pci *vpci,
				   int vec)
{
	struct kvm_msi msi = {
		.address_lo = vpci->msix_table[vec].msg.address_lo,
		.address_hi = vpci->msix_table[vec].msg.address_hi,
		.data = vpci->msix_table[vec].msg.data,
	};

	if (kvm->msix_needs_devid) {
		msi.flags = KVM_MSI_VALID_DEVID;
		msi.devid = vpci->dev_hdr.dev_num << 3;
	}

	irq__signal_msi(kvm, &msi);
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

		if (vpci->signal_msi)
			virtio_pci__signal_msi(kvm, vpci, vpci->vq_vector[vq]);
		else
			kvm__irq_trigger(kvm, vpci->gsis[vq]);
	} else {
		vpci->isr = VIRTIO_IRQ_HIGH;
		kvm__irq_line(kvm, vpci->legacy_irq_line, VIRTIO_IRQ_HIGH);
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

		if (vpci->signal_msi)
			virtio_pci__signal_msi(kvm, vpci, tbl);
		else
			kvm__irq_trigger(kvm, vpci->config_gsi);
	} else {
		vpci->isr = VIRTIO_PCI_ISR_CONFIG;
		kvm__irq_trigger(kvm, vpci->legacy_irq_line);
	}

	return 0;
}

static int virtio_pci__bar_activate(struct kvm *kvm,
				    struct pci_device_header *pci_hdr,
				    int bar_num, void *data)
{
	struct virtio_device *vdev = data;
	mmio_handler_fn mmio_fn;
	u32 bar_addr, bar_size;
	int r = -EINVAL;

	if (vdev->legacy)
		mmio_fn = &virtio_pci_legacy__io_mmio_callback;
	else
		mmio_fn = &virtio_pci_modern__io_mmio_callback;

	assert(bar_num <= 2);

	bar_addr = pci__bar_address(pci_hdr, bar_num);
	bar_size = pci__bar_size(pci_hdr, bar_num);

	switch (bar_num) {
	case 0:
		r = kvm__register_pio(kvm, bar_addr, bar_size, mmio_fn, vdev);
		break;
	case 1:
		r =  kvm__register_mmio(kvm, bar_addr, bar_size, false, mmio_fn,
					vdev);
		break;
	case 2:
		r =  kvm__register_mmio(kvm, bar_addr, bar_size, false,
					virtio_pci__msix_mmio_callback, vdev);
		break;
	}

	return r;
}

static int virtio_pci__bar_deactivate(struct kvm *kvm,
				      struct pci_device_header *pci_hdr,
				      int bar_num, void *data)
{
	u32 bar_addr;
	bool success;
	int r = -EINVAL;

	assert(bar_num <= 2);

	bar_addr = pci__bar_address(pci_hdr, bar_num);

	switch (bar_num) {
	case 0:
		r = kvm__deregister_pio(kvm, bar_addr);
		break;
	case 1:
	case 2:
		success = kvm__deregister_mmio(kvm, bar_addr);
		/* kvm__deregister_mmio fails when the region is not found. */
		r = (success ? 0 : -ENOENT);
		break;
	}

	return r;
}

int virtio_pci__init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class)
{
	struct virtio_pci *vpci = vdev->virtio;
	u32 mmio_addr, msix_io_block;
	u16 port_addr;
	int r;

	vpci->kvm = kvm;
	vpci->dev = dev;

	BUILD_BUG_ON(!is_power_of_two(PCI_IO_SIZE));

	port_addr = pci_get_io_port_block(PCI_IO_SIZE);
	mmio_addr = pci_get_mmio_block(PCI_IO_SIZE);
	msix_io_block = pci_get_mmio_block(VIRTIO_MSIX_BAR_SIZE);

	vpci->pci_hdr = (struct pci_device_header) {
		.vendor_id		= cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET),
		.device_id		= cpu_to_le16(device_id),
		.command		= PCI_COMMAND_IO | PCI_COMMAND_MEMORY,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= vdev->legacy ? 0 : 1,
		.class[0]		= class & 0xff,
		.class[1]		= (class >> 8) & 0xff,
		.class[2]		= (class >> 16) & 0xff,
		.subsys_vendor_id	= cpu_to_le16(PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET),
		.subsys_id		= cpu_to_le16(subsys_id),
		.bar[0]			= cpu_to_le32(port_addr
							| PCI_BASE_ADDRESS_SPACE_IO),
		.bar[1]			= cpu_to_le32(mmio_addr
							| PCI_BASE_ADDRESS_SPACE_MEMORY),
		.bar[2]			= cpu_to_le32(msix_io_block
							| PCI_BASE_ADDRESS_SPACE_MEMORY),
		.status			= cpu_to_le16(PCI_STATUS_CAP_LIST),
		.capabilities		= PCI_CAP_OFF(&vpci->pci_hdr, msix),
		.bar_size[0]		= cpu_to_le32(PCI_IO_SIZE),
		.bar_size[1]		= cpu_to_le32(PCI_IO_SIZE),
		.bar_size[2]		= cpu_to_le32(VIRTIO_MSIX_BAR_SIZE),
	};

	r = pci__register_bar_regions(kvm, &vpci->pci_hdr,
				      virtio_pci__bar_activate,
				      virtio_pci__bar_deactivate, vdev);
	if (r < 0)
		return r;

	vpci->dev_hdr = (struct device_header) {
		.bus_type		= DEVICE_BUS_PCI,
		.data			= &vpci->pci_hdr,
	};

	vpci->pci_hdr.msix.cap = PCI_CAP_ID_MSIX;
	vpci->pci_hdr.msix.next = 0;
	/*
	 * We at most have VIRTIO_NR_MSIX entries (VIRTIO_PCI_MAX_VQ
	 * entries for virt queue, VIRTIO_PCI_MAX_CONFIG entries for
	 * config).
	 *
	 * To quote the PCI spec:
	 *
	 * System software reads this field to determine the
	 * MSI-X Table Size N, which is encoded as N-1.
	 * For example, a returned value of "00000000011"
	 * indicates a table size of 4.
	 */
	vpci->pci_hdr.msix.ctrl = cpu_to_le16(VIRTIO_NR_MSIX - 1);

	/* Both table and PBA are mapped to the same BAR (2) */
	vpci->pci_hdr.msix.table_offset = cpu_to_le32(2);
	vpci->pci_hdr.msix.pba_offset = cpu_to_le32(2 | VIRTIO_MSIX_TABLE_SIZE);
	vpci->config_vector = VIRTIO_MSI_NO_VECTOR;
	/* Initialize all vq vectors to NO_VECTOR */
	memset(vpci->vq_vector, 0xff, sizeof(vpci->vq_vector));

	if (irq__can_signal_msi(kvm))
		vpci->signal_msi = true;

	vpci->legacy_irq_line = pci__assign_irq(&vpci->pci_hdr);

	r = device__register(&vpci->dev_hdr);
	if (r < 0)
		return r;

	if (vdev->legacy)
		vpci->doorbell_offset = VIRTIO_PCI_QUEUE_NOTIFY;
	else
		return virtio_pci_modern_init(vdev);

	return 0;
}

int virtio_pci__reset(struct kvm *kvm, struct virtio_device *vdev)
{
	unsigned int vq;
	struct virtio_pci *vpci = vdev->virtio;

	virtio_pci__del_msix_route(vpci, vpci->config_gsi);
	vpci->config_gsi = 0;
	vpci->config_vector = VIRTIO_MSI_NO_VECTOR;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vpci->dev); vq++)
		virtio_pci_exit_vq(kvm, vdev, vq);

	return 0;
}

int virtio_pci__exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_pci *vpci = vdev->virtio;

	virtio_pci__reset(kvm, vdev);
	kvm__deregister_mmio(kvm, virtio_pci__mmio_addr(vpci));
	kvm__deregister_mmio(kvm, virtio_pci__msix_io_addr(vpci));
	kvm__deregister_pio(kvm, virtio_pci__port_addr(vpci));

	return 0;
}
