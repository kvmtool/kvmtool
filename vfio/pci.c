#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/vfio.h"

#include <sys/ioctl.h>
#include <sys/eventfd.h>

/* Wrapper around UAPI vfio_irq_set */
struct vfio_irq_eventfd {
	struct vfio_irq_set	irq;
	int			fd;
};

static void vfio_pci_cfg_read(struct kvm *kvm, struct pci_device_header *pci_hdr,
			      u8 offset, void *data, int sz)
{
	struct vfio_region_info *info;
	struct vfio_pci_device *pdev;
	struct vfio_device *vdev;
	char base[sz];

	pdev = container_of(pci_hdr, struct vfio_pci_device, hdr);
	vdev = container_of(pdev, struct vfio_device, pci);
	info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;

	/* Dummy read in case of side-effects */
	if (pread(vdev->fd, base, sz, info->offset + offset) != sz)
		vfio_dev_warn(vdev, "failed to read %d bytes from Configuration Space at 0x%x",
			      sz, offset);
}

static void vfio_pci_cfg_write(struct kvm *kvm, struct pci_device_header *pci_hdr,
			       u8 offset, void *data, int sz)
{
	struct vfio_region_info *info;
	struct vfio_pci_device *pdev;
	struct vfio_device *vdev;
	void *base = pci_hdr;

	pdev = container_of(pci_hdr, struct vfio_pci_device, hdr);
	vdev = container_of(pdev, struct vfio_device, pci);
	info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;

	if (pwrite(vdev->fd, data, sz, info->offset + offset) != sz)
		vfio_dev_warn(vdev, "Failed to write %d bytes to Configuration Space at 0x%x",
			      sz, offset);

	if (pread(vdev->fd, base + offset, sz, info->offset + offset) != sz)
		vfio_dev_warn(vdev, "Failed to read %d bytes from Configuration Space at 0x%x",
			      sz, offset);
}

static int vfio_pci_parse_caps(struct vfio_device *vdev)
{
	struct vfio_pci_device *pdev = &vdev->pci;

	if (!(pdev->hdr.status & PCI_STATUS_CAP_LIST))
		return 0;

	pdev->hdr.status &= ~PCI_STATUS_CAP_LIST;
	pdev->hdr.capabilities = 0;

	/* TODO: install virtual capabilities */

	return 0;
}

static int vfio_pci_parse_cfg_space(struct vfio_device *vdev)
{
	ssize_t sz = PCI_STD_HEADER_SIZEOF;
	struct vfio_region_info *info;
	struct vfio_pci_device *pdev = &vdev->pci;

	if (vdev->info.num_regions < VFIO_PCI_CONFIG_REGION_INDEX) {
		vfio_dev_err(vdev, "Config Space not found");
		return -ENODEV;
	}

	info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;
	*info = (struct vfio_region_info) {
			.argsz = sizeof(*info),
			.index = VFIO_PCI_CONFIG_REGION_INDEX,
	};

	ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, info);
	if (!info->size) {
		vfio_dev_err(vdev, "Config Space has size zero?!");
		return -EINVAL;
	}

	if (pread(vdev->fd, &pdev->hdr, sz, info->offset) != sz) {
		vfio_dev_err(vdev, "failed to read %zd bytes of Config Space", sz);
		return -EIO;
	}

	/* Strip bit 7, that indicates multifunction */
	pdev->hdr.header_type &= 0x7f;

	if (pdev->hdr.header_type != PCI_HEADER_TYPE_NORMAL) {
		vfio_dev_err(vdev, "unsupported header type %u",
			     pdev->hdr.header_type);
		return -EOPNOTSUPP;
	}

	vfio_pci_parse_caps(vdev);

	return 0;
}

static int vfio_pci_fixup_cfg_space(struct vfio_device *vdev)
{
	int i;
	ssize_t hdr_sz;
	struct vfio_region_info *info;
	struct vfio_pci_device *pdev = &vdev->pci;

	/* Enable exclusively MMIO and bus mastering */
	pdev->hdr.command &= ~PCI_COMMAND_IO;
	pdev->hdr.command |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;

	/* Initialise the BARs */
	for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
		struct vfio_region *region = &vdev->regions[i];
		u64 base = region->guest_phys_addr;

		if (!base)
			continue;

		pdev->hdr.bar_size[i] = region->info.size;

		/* Construct a fake reg to match what we've mapped. */
		pdev->hdr.bar[i] = (base & PCI_BASE_ADDRESS_MEM_MASK) |
					PCI_BASE_ADDRESS_SPACE_MEMORY |
					PCI_BASE_ADDRESS_MEM_TYPE_32;
	}

	/* I really can't be bothered to support cardbus. */
	pdev->hdr.card_bus = 0;

	/*
	 * Nuke the expansion ROM for now. If we want to do this properly,
	 * we need to save its size somewhere and map into the guest.
	 */
	pdev->hdr.exp_rom_bar = 0;

	/* Install our fake Configuration Space */
	info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;
	hdr_sz = PCI_DEV_CFG_SIZE;
	if (pwrite(vdev->fd, &pdev->hdr, hdr_sz, info->offset) != hdr_sz) {
		vfio_dev_err(vdev, "failed to write %zd bytes to Config Space",
			     hdr_sz);
		return -EIO;
	}

	/* Register callbacks for cfg accesses */
	pdev->hdr.cfg_ops = (struct pci_config_operations) {
		.read	= vfio_pci_cfg_read,
		.write	= vfio_pci_cfg_write,
	};

	pdev->hdr.irq_type = IRQ_TYPE_LEVEL_HIGH;

	return 0;
}

static int vfio_pci_configure_bar(struct kvm *kvm, struct vfio_device *vdev,
				  size_t nr)
{
	int ret;
	size_t map_size;
	struct vfio_region *region = &vdev->regions[nr];

	if (nr >= vdev->info.num_regions)
		return 0;

	region->info = (struct vfio_region_info) {
		.argsz = sizeof(region->info),
		.index = nr,
	};

	ret = ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, &region->info);
	if (ret) {
		ret = -errno;
		vfio_dev_err(vdev, "cannot get info for BAR %zu", nr);
		return ret;
	}

	/* Ignore invalid or unimplemented regions */
	if (!region->info.size)
		return 0;

	/* Grab some MMIO space in the guest */
	map_size = ALIGN(region->info.size, PAGE_SIZE);
	region->guest_phys_addr = pci_get_io_space_block(map_size);

	/*
	 * Map the BARs into the guest. We'll later need to update
	 * configuration space to reflect our allocation.
	 */
	ret = vfio_map_region(kvm, vdev, region);
	if (ret)
		return ret;

	return 0;
}

static int vfio_pci_configure_dev_regions(struct kvm *kvm,
					  struct vfio_device *vdev)
{
	int ret;
	u32 bar;
	size_t i;
	bool is_64bit = false;
	struct vfio_pci_device *pdev = &vdev->pci;

	ret = vfio_pci_parse_cfg_space(vdev);
	if (ret)
		return ret;

	for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
		/* Ignore top half of 64-bit BAR */
		if (i % 2 && is_64bit)
			continue;

		ret = vfio_pci_configure_bar(kvm, vdev, i);
		if (ret)
			return ret;

		bar = pdev->hdr.bar[i];
		is_64bit = (bar & PCI_BASE_ADDRESS_SPACE) ==
			   PCI_BASE_ADDRESS_SPACE_MEMORY &&
			   bar & PCI_BASE_ADDRESS_MEM_TYPE_64;
	}

	/* We've configured the BARs, fake up a Configuration Space */
	return vfio_pci_fixup_cfg_space(vdev);
}

static int vfio_pci_enable_intx(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	int trigger_fd, unmask_fd;
	struct vfio_irq_eventfd	trigger;
	struct vfio_irq_eventfd	unmask;
	struct vfio_pci_device *pdev = &vdev->pci;
	int gsi = pdev->hdr.irq_line - KVM_IRQ_OFFSET;

	struct vfio_irq_info irq_info = {
		.argsz = sizeof(irq_info),
		.index = VFIO_PCI_INTX_IRQ_INDEX,
	};

	ret = ioctl(vdev->fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
	if (ret || irq_info.count == 0) {
		vfio_dev_err(vdev, "no INTx reported by VFIO");
		return -ENODEV;
	}

	if (!(irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
		vfio_dev_err(vdev, "interrupt not eventfd capable");
		return -EINVAL;
	}

	if (!(irq_info.flags & VFIO_IRQ_INFO_AUTOMASKED)) {
		vfio_dev_err(vdev, "INTx interrupt not AUTOMASKED");
		return -EINVAL;
	}

	/*
	 * PCI IRQ is level-triggered, so we use two eventfds. trigger_fd
	 * signals an interrupt from host to guest, and unmask_fd signals the
	 * deassertion of the line from guest to host.
	 */
	trigger_fd = eventfd(0, 0);
	if (trigger_fd < 0) {
		vfio_dev_err(vdev, "failed to create trigger eventfd");
		return trigger_fd;
	}

	unmask_fd = eventfd(0, 0);
	if (unmask_fd < 0) {
		vfio_dev_err(vdev, "failed to create unmask eventfd");
		close(trigger_fd);
		return unmask_fd;
	}

	ret = irq__add_irqfd(kvm, gsi, trigger_fd, unmask_fd);
	if (ret)
		goto err_close;

	trigger.irq = (struct vfio_irq_set) {
		.argsz	= sizeof(trigger),
		.flags	= VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index	= VFIO_PCI_INTX_IRQ_INDEX,
		.start	= 0,
		.count	= 1,
	};
	trigger.fd = trigger_fd;

	ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);
	if (ret < 0) {
		vfio_dev_err(vdev, "failed to setup VFIO IRQ");
		goto err_delete_line;
	}

	unmask.irq = (struct vfio_irq_set) {
		.argsz	= sizeof(unmask),
		.flags	= VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_UNMASK,
		.index	= VFIO_PCI_INTX_IRQ_INDEX,
		.start	= 0,
		.count	= 1,
	};
	unmask.fd = unmask_fd;

	ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
	if (ret < 0) {
		vfio_dev_err(vdev, "failed to setup unmask IRQ");
		goto err_remove_event;
	}

	return 0;

err_remove_event:
	/* Remove trigger event */
	trigger.irq.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	trigger.irq.count = 0;
	ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);

err_delete_line:
	irq__del_irqfd(kvm, gsi, trigger_fd);

err_close:
	close(trigger_fd);
	close(unmask_fd);
	return ret;
}

static int vfio_pci_configure_dev_irqs(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_pci_device *pdev = &vdev->pci;

	struct vfio_irq_info irq_info = {
		.argsz = sizeof(irq_info),
		.index = VFIO_PCI_INTX_IRQ_INDEX,
	};

	if (!pdev->hdr.irq_pin) {
		/* TODO: add MSI support */
		vfio_dev_err(vdev, "INTx not available, MSI-X not implemented");
		return -ENOSYS;
	}

	return vfio_pci_enable_intx(kvm, vdev);
}

int vfio_pci_setup_device(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;

	ret = vfio_pci_configure_dev_regions(kvm, vdev);
	if (ret) {
		vfio_dev_err(vdev, "failed to configure regions");
		return ret;
	}

	vdev->dev_hdr = (struct device_header) {
		.bus_type	= DEVICE_BUS_PCI,
		.data		= &vdev->pci.hdr,
	};

	ret = device__register(&vdev->dev_hdr);
	if (ret) {
		vfio_dev_err(vdev, "failed to register VFIO device");
		return ret;
	}

	ret = vfio_pci_configure_dev_irqs(kvm, vdev);
	if (ret) {
		vfio_dev_err(vdev, "failed to configure IRQs");
		return ret;
	}

	return 0;
}

void vfio_pci_teardown_device(struct kvm *kvm, struct vfio_device *vdev)
{
	size_t i;

	for (i = 0; i < vdev->info.num_regions; i++)
		vfio_unmap_region(kvm, &vdev->regions[i]);

	device__unregister(&vdev->dev_hdr);
}
