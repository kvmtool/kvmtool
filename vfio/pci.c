#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/vfio.h"

#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/time.h>

/* Wrapper around UAPI vfio_irq_set */
union vfio_irq_eventfd {
	struct vfio_irq_set	irq;
	u8 buffer[sizeof(struct vfio_irq_set) + sizeof(int)];
};

static void set_vfio_irq_eventd_payload(union vfio_irq_eventfd *evfd, int fd)
{
	memcpy(&evfd->irq.data, &fd, sizeof(fd));
}

#define msi_is_enabled(state)		((state) & VFIO_PCI_MSI_STATE_ENABLED)
#define msi_is_masked(state)		((state) & VFIO_PCI_MSI_STATE_MASKED)
#define msi_is_empty(state)		((state) & VFIO_PCI_MSI_STATE_EMPTY)

#define msi_update_state(state, val, bit)				\
	(state) = (val) ? (state) | bit : (state) & ~bit;
#define msi_set_enabled(state, val)					\
	msi_update_state(state, val, VFIO_PCI_MSI_STATE_ENABLED)
#define msi_set_masked(state, val)					\
	msi_update_state(state, val, VFIO_PCI_MSI_STATE_MASKED)
#define msi_set_empty(state, val)					\
	msi_update_state(state, val, VFIO_PCI_MSI_STATE_EMPTY)

static void vfio_pci_disable_intx(struct kvm *kvm, struct vfio_device *vdev);
static int vfio_pci_enable_intx(struct kvm *kvm, struct vfio_device *vdev);

static int vfio_pci_enable_msis(struct kvm *kvm, struct vfio_device *vdev,
				bool msix)
{
	size_t i;
	int ret = 0;
	int *eventfds;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct vfio_pci_msi_common *msis = msix ? &pdev->msix : &pdev->msi;
	union vfio_irq_eventfd single = {
		.irq = {
			.argsz	= sizeof(single),
			.flags	= VFIO_IRQ_SET_DATA_EVENTFD |
				  VFIO_IRQ_SET_ACTION_TRIGGER,
			.index	= msis->info.index,
			.count	= 1,
		},
	};

	if (!msi_is_enabled(msis->virt_state))
		return 0;

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_INTX)
		/*
		 * PCI (and VFIO) forbids enabling INTx, MSI or MSIX at the same
		 * time. Since INTx has to be enabled from the start (we don't
		 * have a reliable way to know when the guest starts using it),
		 * disable it now.
		 */
		vfio_pci_disable_intx(kvm, vdev);

	eventfds = (void *)msis->irq_set + sizeof(struct vfio_irq_set);

	/*
	 * Initial registration of the full range. This enables the physical
	 * MSI/MSI-X capability, which might have desired side effects. For
	 * instance when assigning virtio legacy devices, enabling the MSI
	 * capability modifies the config space layout!
	 *
	 * As an optimization, only update MSIs when guest unmasks the
	 * capability. This greatly reduces the initialization time for Linux
	 * guest with 2048+ MSIs. Linux guest starts by enabling the MSI-X cap
	 * masked, then fills individual vectors, then unmasks the whole
	 * function. So we only do one VFIO ioctl when enabling for the first
	 * time, and then one when unmasking.
	 *
	 * phys_state is empty when it is enabled but no vector has been
	 * registered via SET_IRQS yet.
	 */
	if (!msi_is_enabled(msis->phys_state) ||
	    (!msi_is_masked(msis->virt_state) &&
	     msi_is_empty(msis->phys_state))) {
		bool empty = true;

		for (i = 0; i < msis->nr_entries; i++) {
			eventfds[i] = msis->entries[i].gsi >= 0 ?
				      msis->entries[i].eventfd : -1;

			if (eventfds[i] >= 0)
				empty = false;
		}

		ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, msis->irq_set);
		if (ret < 0) {
			perror("VFIO_DEVICE_SET_IRQS(multi)");
			return ret;
		}

		msi_set_enabled(msis->phys_state, true);
		msi_set_empty(msis->phys_state, empty);

		return 0;
	}

	if (msi_is_masked(msis->virt_state)) {
		/* TODO: if phys_state is not empty nor masked, mask all vectors */
		return 0;
	}

	/* Update individual vectors to avoid breaking those in use */
	for (i = 0; i < msis->nr_entries; i++) {
		struct vfio_pci_msi_entry *entry = &msis->entries[i];
		int fd = entry->gsi >= 0 ? entry->eventfd : -1;

		if (fd == eventfds[i])
			continue;

		single.irq.start = i;
		set_vfio_irq_eventd_payload(&single, fd);

		ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &single);
		if (ret < 0) {
			perror("VFIO_DEVICE_SET_IRQS(single)");
			break;
		}

		eventfds[i] = fd;

		if (msi_is_empty(msis->phys_state) && fd >= 0)
			msi_set_empty(msis->phys_state, false);
	}

	return ret;
}

static int vfio_pci_disable_msis(struct kvm *kvm, struct vfio_device *vdev,
				 bool msix)
{
	int ret;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct vfio_pci_msi_common *msis = msix ? &pdev->msix : &pdev->msi;
	struct vfio_irq_set irq_set = {
		.argsz	= sizeof(irq_set),
		.flags 	= VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index 	= msis->info.index,
		.start 	= 0,
		.count	= 0,
	};

	if (!msi_is_enabled(msis->phys_state))
		return 0;

	ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);
	if (ret < 0) {
		perror("VFIO_DEVICE_SET_IRQS(NONE)");
		return ret;
	}

	msi_set_enabled(msis->phys_state, false);
	msi_set_empty(msis->phys_state, true);

	/*
	 * When MSI or MSIX is disabled, this might be called when
	 * PCI driver detects the MSI interrupt failure and wants to
	 * rollback to INTx mode.  Thus enable INTx if the device
	 * supports INTx mode in this case.
	 */
	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_INTX)
		ret = vfio_pci_enable_intx(kvm, vdev);

	return ret >= 0 ? 0 : ret;
}

static int vfio_pci_update_msi_entry(struct kvm *kvm, struct vfio_device *vdev,
				     struct vfio_pci_msi_entry *entry)
{
	int ret;

	if (entry->eventfd < 0) {
		entry->eventfd = eventfd(0, 0);
		if (entry->eventfd < 0) {
			ret = -errno;
			vfio_dev_err(vdev, "cannot create eventfd");
			return ret;
		}
	}

	/* Allocate IRQ if necessary */
	if (entry->gsi < 0) {
		int ret = irq__add_msix_route(kvm, &entry->config.msg,
					      vdev->dev_hdr.dev_num << 3);
		if (ret < 0) {
			vfio_dev_err(vdev, "cannot create MSI-X route");
			return ret;
		}
		entry->gsi = ret;
	} else {
		irq__update_msix_route(kvm, entry->gsi, &entry->config.msg);
	}

	/*
	 * MSI masking is unimplemented in VFIO, so we have to handle it by
	 * disabling/enabling IRQ route instead. We do it on the KVM side rather
	 * than VFIO, because:
	 * - it is 8x faster
	 * - it allows to decouple masking logic from capability state.
	 * - in masked state, after removing irqfd route, we could easily plug
	 *   the eventfd in a local handler, in order to serve Pending Bit reads
	 *   to the guest.
	 *
	 * So entry->phys_state is masked when there is no active irqfd route.
	 */
	if (msi_is_masked(entry->virt_state) == msi_is_masked(entry->phys_state))
		return 0;

	if (msi_is_masked(entry->phys_state)) {
		ret = irq__add_irqfd(kvm, entry->gsi, entry->eventfd, -1);
		if (ret < 0) {
			vfio_dev_err(vdev, "cannot setup irqfd");
			return ret;
		}
	} else {
		irq__del_irqfd(kvm, entry->gsi, entry->eventfd);
	}

	msi_set_masked(entry->phys_state, msi_is_masked(entry->virt_state));

	return 0;
}

static void vfio_pci_msix_pba_access(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				     u32 len, u8 is_write, void *ptr)
{
	struct vfio_pci_device *pdev = ptr;
	struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
	u64 offset = addr - pba->guest_phys_addr;
	struct vfio_device *vdev = container_of(pdev, struct vfio_device, pci);

	if (is_write)
		return;

	/*
	 * TODO: emulate PBA. Hardware MSI-X is never masked, so reading the PBA
	 * is completely useless here. Note that Linux doesn't use PBA.
	 */
	if (pread(vdev->fd, data, len, pba->offset + offset) != (ssize_t)len)
		vfio_dev_err(vdev, "cannot access MSIX PBA\n");
}

static void vfio_pci_msix_table_access(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				       u32 len, u8 is_write, void *ptr)
{
	struct kvm *kvm = vcpu->kvm;
	struct vfio_pci_msi_entry *entry;
	struct vfio_pci_device *pdev = ptr;
	struct vfio_device *vdev = container_of(pdev, struct vfio_device, pci);

	u64 offset = addr - pdev->msix_table.guest_phys_addr;

	size_t vector = offset / PCI_MSIX_ENTRY_SIZE;
	off_t field = offset % PCI_MSIX_ENTRY_SIZE;

	/*
	 * PCI spec says that software must use aligned 4 or 8 bytes accesses
	 * for the MSI-X tables.
	 */
	if ((len != 4 && len != 8) || addr & (len - 1)) {
		vfio_dev_warn(vdev, "invalid MSI-X table access");
		return;
	}

	entry = &pdev->msix.entries[vector];

	mutex_lock(&pdev->msix.mutex);

	if (!is_write) {
		memcpy(data, (void *)&entry->config + field, len);
		goto out_unlock;
	}

	memcpy((void *)&entry->config + field, data, len);

	/*
	 * Check if access touched the vector control register, which is at the
	 * end of the MSI-X entry.
	 */
	if (field + len <= PCI_MSIX_ENTRY_VECTOR_CTRL)
		goto out_unlock;

	msi_set_masked(entry->virt_state, entry->config.ctrl &
		       PCI_MSIX_ENTRY_CTRL_MASKBIT);

	if (vfio_pci_update_msi_entry(kvm, vdev, entry) < 0)
		/* Not much we can do here. */
		vfio_dev_err(vdev, "failed to configure MSIX vector %zu", vector);

	/* Update the physical capability if necessary */
	if (vfio_pci_enable_msis(kvm, vdev, true))
		vfio_dev_err(vdev, "cannot enable MSIX");

out_unlock:
	mutex_unlock(&pdev->msix.mutex);
}

static void vfio_pci_msix_cap_write(struct kvm *kvm,
				    struct vfio_device *vdev, u8 off,
				    void *data, int sz)
{
	struct vfio_pci_device *pdev = &vdev->pci;
	off_t enable_pos = PCI_MSIX_FLAGS + 1;
	bool enable;
	u16 flags;

	off -= pdev->msix.pos;

	/* Check if access intersects with the MSI-X Enable bit */
	if (off > enable_pos || off + sz <= enable_pos)
		return;

	/* Read byte that contains the Enable bit */
	flags = *(u8 *)(data + enable_pos - off) << 8;

	mutex_lock(&pdev->msix.mutex);

	msi_set_masked(pdev->msix.virt_state, flags & PCI_MSIX_FLAGS_MASKALL);
	enable = flags & PCI_MSIX_FLAGS_ENABLE;
	msi_set_enabled(pdev->msix.virt_state, enable);

	if (enable && vfio_pci_enable_msis(kvm, vdev, true))
		vfio_dev_err(vdev, "cannot enable MSIX");
	else if (!enable && vfio_pci_disable_msis(kvm, vdev, true))
		vfio_dev_err(vdev, "cannot disable MSIX");

	mutex_unlock(&pdev->msix.mutex);
}

static int vfio_pci_msi_vector_write(struct kvm *kvm, struct vfio_device *vdev,
				     u8 off, u8 *data, u32 sz)
{
	size_t i;
	u32 mask = 0;
	size_t mask_pos, start, limit;
	struct vfio_pci_msi_entry *entry;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct msi_cap_64 *msi_cap_64 = PCI_CAP(&pdev->hdr, pdev->msi.pos);

	if (!(msi_cap_64->ctrl & PCI_MSI_FLAGS_MASKBIT))
		return 0;

	if (msi_cap_64->ctrl & PCI_MSI_FLAGS_64BIT)
		mask_pos = PCI_MSI_MASK_64;
	else
		mask_pos = PCI_MSI_MASK_32;

	if (off >= mask_pos + 4 || off + sz <= mask_pos)
		return 0;

	/* Set mask to current state */
	for (i = 0; i < pdev->msi.nr_entries; i++) {
		entry = &pdev->msi.entries[i];
		mask |= !!msi_is_masked(entry->virt_state) << i;
	}

	/* Update mask following the intersection of access and register */
	start = max_t(size_t, off, mask_pos);
	limit = min_t(size_t, off + sz, mask_pos + 4);

	memcpy((void *)&mask + start - mask_pos, data + start - off,
	       limit - start);

	/* Update states if necessary */
	for (i = 0; i < pdev->msi.nr_entries; i++) {
		bool masked = mask & (1 << i);

		entry = &pdev->msi.entries[i];
		if (masked != msi_is_masked(entry->virt_state)) {
			msi_set_masked(entry->virt_state, masked);
			vfio_pci_update_msi_entry(kvm, vdev, entry);
		}
	}

	return 1;
}

static void vfio_pci_msi_cap_write(struct kvm *kvm, struct vfio_device *vdev,
				   u8 off, u8 *data, u32 sz)
{
	u8 ctrl;
	struct msi_msg msg;
	size_t i, nr_vectors;
	struct vfio_pci_msi_entry *entry;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct msi_cap_64 *msi_cap_64 = PCI_CAP(&pdev->hdr, pdev->msi.pos);

	off -= pdev->msi.pos;

	mutex_lock(&pdev->msi.mutex);

	/* Check if the guest is trying to update mask bits */
	if (vfio_pci_msi_vector_write(kvm, vdev, off, data, sz))
		goto out_unlock;

	/* Only modify routes when guest pokes the enable bit */
	if (off > PCI_MSI_FLAGS || off + sz <= PCI_MSI_FLAGS)
		goto out_unlock;

	ctrl = *(u8 *)(data + PCI_MSI_FLAGS - off);

	msi_set_enabled(pdev->msi.virt_state, ctrl & PCI_MSI_FLAGS_ENABLE);

	if (!msi_is_enabled(pdev->msi.virt_state)) {
		vfio_pci_disable_msis(kvm, vdev, false);
		goto out_unlock;
	}

	/* Create routes for the requested vectors */
	nr_vectors = 1 << ((ctrl & PCI_MSI_FLAGS_QSIZE) >> 4);

	msg.address_lo = msi_cap_64->address_lo;
	if (msi_cap_64->ctrl & PCI_MSI_FLAGS_64BIT) {
		msg.address_hi = msi_cap_64->address_hi;
		msg.data = msi_cap_64->data;
	} else {
		struct msi_cap_32 *msi_cap_32 = (void *)msi_cap_64;
		msg.address_hi = 0;
		msg.data = msi_cap_32->data;
	}

	for (i = 0; i < nr_vectors; i++) {
		entry = &pdev->msi.entries[i];
		entry->config.msg = msg;
		vfio_pci_update_msi_entry(kvm, vdev, entry);
	}

	/* Update the physical capability if necessary */
	if (vfio_pci_enable_msis(kvm, vdev, false))
		vfio_dev_err(vdev, "cannot enable MSI");

out_unlock:
	mutex_unlock(&pdev->msi.mutex);
}

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

	if (offset == PCI_ROM_ADDRESS)
		return;

	pdev = container_of(pci_hdr, struct vfio_pci_device, hdr);
	vdev = container_of(pdev, struct vfio_device, pci);
	info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;

	if (pwrite(vdev->fd, data, sz, info->offset + offset) != sz)
		vfio_dev_warn(vdev, "Failed to write %d bytes to Configuration Space at 0x%x",
			      sz, offset);

	/* Handle MSI write now, since it might update the hardware capability */
	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX)
		vfio_pci_msix_cap_write(kvm, vdev, offset, data, sz);

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI)
		vfio_pci_msi_cap_write(kvm, vdev, offset, data, sz);

	if (pread(vdev->fd, base + offset, sz, info->offset + offset) != sz)
		vfio_dev_warn(vdev, "Failed to read %d bytes from Configuration Space at 0x%x",
			      sz, offset);
}

static ssize_t vfio_pci_msi_cap_size(struct msi_cap_64 *cap_hdr)
{
	size_t size = 10;

	if (cap_hdr->ctrl & PCI_MSI_FLAGS_64BIT)
		size += 4;
	if (cap_hdr->ctrl & PCI_MSI_FLAGS_MASKBIT)
		size += 10;

	return size;
}

static ssize_t vfio_pci_cap_size(struct pci_cap_hdr *cap_hdr)
{
	switch (cap_hdr->type) {
	case PCI_CAP_ID_MSIX:
		return PCI_CAP_MSIX_SIZEOF;
	case PCI_CAP_ID_MSI:
		return vfio_pci_msi_cap_size((void *)cap_hdr);
	default:
		pr_err("unknown PCI capability 0x%x", cap_hdr->type);
		return 0;
	}
}

static int vfio_pci_add_cap(struct vfio_device *vdev, u8 *virt_hdr,
			    struct pci_cap_hdr *cap, off_t pos)
{
	struct pci_cap_hdr *last;
	struct pci_device_header *hdr = &vdev->pci.hdr;

	cap->next = 0;

	if (!hdr->capabilities) {
		hdr->capabilities = pos;
		hdr->status |= PCI_STATUS_CAP_LIST;
	} else {
		last = PCI_CAP(virt_hdr, hdr->capabilities);

		while (last->next)
			last = PCI_CAP(virt_hdr, last->next);

		last->next = pos;
	}

	memcpy(virt_hdr + pos, cap, vfio_pci_cap_size(cap));

	return 0;
}

static int vfio_pci_parse_caps(struct vfio_device *vdev)
{
	int ret;
	size_t size;
	u8 pos, next;
	struct pci_cap_hdr *cap;
	u8 virt_hdr[PCI_DEV_CFG_SIZE];
	struct vfio_pci_device *pdev = &vdev->pci;

	if (!(pdev->hdr.status & PCI_STATUS_CAP_LIST))
		return 0;

	memset(virt_hdr, 0, PCI_DEV_CFG_SIZE);

	pos = pdev->hdr.capabilities & ~3;

	pdev->hdr.status &= ~PCI_STATUS_CAP_LIST;
	pdev->hdr.capabilities = 0;

	for (; pos; pos = next) {
		cap = PCI_CAP(&pdev->hdr, pos);
		next = cap->next;

		switch (cap->type) {
		case PCI_CAP_ID_MSIX:
			ret = vfio_pci_add_cap(vdev, virt_hdr, cap, pos);
			if (ret)
				return ret;

			pdev->msix.pos = pos;
			pdev->irq_modes |= VFIO_PCI_IRQ_MODE_MSIX;
			break;
		case PCI_CAP_ID_MSI:
			ret = vfio_pci_add_cap(vdev, virt_hdr, cap, pos);
			if (ret)
				return ret;

			pdev->msi.pos = pos;
			pdev->irq_modes |= VFIO_PCI_IRQ_MODE_MSI;
			break;
		}
	}

	/* Wipe remaining capabilities */
	pos = PCI_STD_HEADER_SIZEOF;
	size = PCI_DEV_CFG_SIZE - PCI_STD_HEADER_SIZEOF;
	memcpy((void *)&pdev->hdr + pos, virt_hdr + pos, size);

	return 0;
}

static int vfio_pci_parse_cfg_space(struct vfio_device *vdev)
{
	ssize_t sz = PCI_DEV_CFG_SIZE;
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

	/* Read standard headers and capabilities */
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

	if (pdev->hdr.irq_pin)
		pdev->irq_modes |= VFIO_PCI_IRQ_MODE_INTX;

	vfio_pci_parse_caps(vdev);

	return 0;
}

static int vfio_pci_fixup_cfg_space(struct vfio_device *vdev)
{
	int i;
	u64 base;
	ssize_t hdr_sz;
	struct msix_cap *msix;
	struct vfio_region_info *info;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct vfio_region *region;

	/* Initialise the BARs */
	for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
		if ((u32)i == vdev->info.num_regions)
			break;

		region = &vdev->regions[i];
		/* Construct a fake reg to match what we've mapped. */
		if (region->is_ioport) {
			base = (region->port_base & PCI_BASE_ADDRESS_IO_MASK) |
				PCI_BASE_ADDRESS_SPACE_IO;
		} else {
			base = (region->guest_phys_addr &
				PCI_BASE_ADDRESS_MEM_MASK) |
				PCI_BASE_ADDRESS_SPACE_MEMORY;
		}

		pdev->hdr.bar[i] = base;

		if (!base)
			continue;

		pdev->hdr.bar_size[i] = region->info.size;
	}

	/* I really can't be bothered to support cardbus. */
	pdev->hdr.card_bus = 0;

	/*
	 * Nuke the expansion ROM for now. If we want to do this properly,
	 * we need to save its size somewhere and map into the guest.
	 */
	pdev->hdr.exp_rom_bar = 0;

	/* Plumb in our fake MSI-X capability, if we have it. */
	msix = pci_find_cap(&pdev->hdr, PCI_CAP_ID_MSIX);
	if (msix) {
		/* Add a shortcut to the PBA region for the MMIO handler */
		int pba_index = VFIO_PCI_BAR0_REGION_INDEX + pdev->msix_pba.bar;
		pdev->msix_pba.offset = vdev->regions[pba_index].info.offset +
					(msix->pba_offset & PCI_MSIX_PBA_OFFSET);

		/* Tidy up the capability */
		msix->table_offset &= PCI_MSIX_TABLE_BIR;
		msix->pba_offset &= PCI_MSIX_PBA_BIR;
		if (pdev->msix_table.bar == pdev->msix_pba.bar)
			msix->pba_offset |= pdev->msix_table.size &
					    PCI_MSIX_PBA_OFFSET;
	}

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

static int vfio_pci_get_region_info(struct vfio_device *vdev, u32 index,
				    struct vfio_region_info *info)
{
	int ret;

	*info = (struct vfio_region_info) {
		.argsz = sizeof(*info),
		.index = index,
	};

	ret = ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, info);
	if (ret) {
		ret = -errno;
		vfio_dev_err(vdev, "cannot get info for BAR %u", index);
		return ret;
	}

	if (info->size && !is_power_of_two(info->size)) {
		vfio_dev_err(vdev, "region is not power of two: 0x%llx",
				info->size);
		return -EINVAL;
	}

	return 0;
}

static int vfio_pci_create_msix_table(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	size_t i;
	size_t map_size;
	size_t nr_entries;
	struct vfio_pci_msi_entry *entries;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
	struct vfio_pci_msix_table *table = &pdev->msix_table;
	struct msix_cap *msix = PCI_CAP(&pdev->hdr, pdev->msix.pos);
	struct vfio_region_info info;

	table->bar = msix->table_offset & PCI_MSIX_TABLE_BIR;
	pba->bar = msix->pba_offset & PCI_MSIX_TABLE_BIR;

	/*
	 * KVM needs memory regions to be multiple of and aligned on PAGE_SIZE.
	 */
	nr_entries = (msix->ctrl & PCI_MSIX_FLAGS_QSIZE) + 1;
	table->size = ALIGN(nr_entries * PCI_MSIX_ENTRY_SIZE, PAGE_SIZE);
	pba->size = ALIGN(DIV_ROUND_UP(nr_entries, 64), PAGE_SIZE);

	entries = calloc(nr_entries, sizeof(struct vfio_pci_msi_entry));
	if (!entries)
		return -ENOMEM;

	for (i = 0; i < nr_entries; i++)
		entries[i].config.ctrl = PCI_MSIX_ENTRY_CTRL_MASKBIT;

	ret = vfio_pci_get_region_info(vdev, table->bar, &info);
	if (ret)
		return ret;
	if (!info.size)
		return -EINVAL;
	map_size = info.size;

	if (table->bar != pba->bar) {
		ret = vfio_pci_get_region_info(vdev, pba->bar, &info);
		if (ret)
			return ret;
		if (!info.size)
			return -EINVAL;
		map_size += info.size;
	}

	/*
	 * To ease MSI-X cap configuration in case they share the same BAR,
	 * collapse table and pending array. The size of the BAR regions must be
	 * powers of two.
	 */
	map_size = ALIGN(map_size, PAGE_SIZE);
	table->guest_phys_addr = pci_get_mmio_block(map_size);
	if (!table->guest_phys_addr) {
		pr_err("cannot allocate MMIO space");
		ret = -ENOMEM;
		goto out_free;
	}
	pba->guest_phys_addr = table->guest_phys_addr + table->size;

	ret = kvm__register_mmio(kvm, table->guest_phys_addr, table->size,
				 false, vfio_pci_msix_table_access, pdev);
	if (ret < 0)
		goto out_free;

	/*
	 * We could map the physical PBA directly into the guest, but it's
	 * likely smaller than a page, and we can only hand full pages to the
	 * guest. Even though the PCI spec disallows sharing a page used for
	 * MSI-X with any other resource, it allows to share the same page
	 * between MSI-X table and PBA. For the sake of isolation, create a
	 * virtual PBA.
	 */
	ret = kvm__register_mmio(kvm, pba->guest_phys_addr, pba->size, false,
				 vfio_pci_msix_pba_access, pdev);
	if (ret < 0)
		goto out_free;

	pdev->msix.entries = entries;
	pdev->msix.nr_entries = nr_entries;

	return 0;

out_free:
	free(entries);

	return ret;
}

static int vfio_pci_create_msi_cap(struct kvm *kvm, struct vfio_pci_device *pdev)
{
	struct msi_cap_64 *cap = PCI_CAP(&pdev->hdr, pdev->msi.pos);

	pdev->msi.nr_entries = 1 << ((cap->ctrl & PCI_MSI_FLAGS_QMASK) >> 1),
	pdev->msi.entries = calloc(pdev->msi.nr_entries,
				   sizeof(struct vfio_pci_msi_entry));
	if (!pdev->msi.entries)
		return -ENOMEM;

	return 0;
}

static int vfio_pci_configure_bar(struct kvm *kvm, struct vfio_device *vdev,
				  size_t nr)
{
	int ret;
	u32 bar;
	size_t map_size;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct vfio_region *region;

	if (nr >= vdev->info.num_regions)
		return 0;

	region = &vdev->regions[nr];
	bar = pdev->hdr.bar[nr];

	region->vdev = vdev;
	region->is_ioport = !!(bar & PCI_BASE_ADDRESS_SPACE_IO);

	ret = vfio_pci_get_region_info(vdev, nr, &region->info);
	if (ret)
		return ret;

	/* Ignore invalid or unimplemented regions */
	if (!region->info.size)
		return 0;

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
		/* Trap and emulate MSI-X table */
		if (nr == pdev->msix_table.bar) {
			region->guest_phys_addr = pdev->msix_table.guest_phys_addr;
			return 0;
		} else if (nr == pdev->msix_pba.bar) {
			region->guest_phys_addr = pdev->msix_pba.guest_phys_addr;
			return 0;
		}
	}

	if (!region->is_ioport) {
		/* Grab some MMIO space in the guest */
		map_size = ALIGN(region->info.size, PAGE_SIZE);
		region->guest_phys_addr = pci_get_mmio_block(map_size);
	}

	/* Map the BARs into the guest or setup a trap region. */
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

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
		ret = vfio_pci_create_msix_table(kvm, vdev);
		if (ret)
			return ret;
	}

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI) {
		ret = vfio_pci_create_msi_cap(kvm, pdev);
		if (ret)
			return ret;
	}

	for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
		/* Ignore top half of 64-bit BAR */
		if (is_64bit) {
			is_64bit = false;
			continue;
		}

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

/*
 * Attempt to update the FD limit, if opening an eventfd for each IRQ vector
 * would hit the limit. Which is likely to happen when a device uses 2048 MSIs.
 */
static int vfio_pci_reserve_irq_fds(size_t num)
{
	/*
	 * I counted around 27 fds under normal load. Let's add 100 for good
	 * measure.
	 */
	static size_t needed = 128;
	struct rlimit fd_limit, new_limit;

	needed += num;

	if (getrlimit(RLIMIT_NOFILE, &fd_limit)) {
		perror("getrlimit(RLIMIT_NOFILE)");
		return 0;
	}

	if (fd_limit.rlim_cur >= needed)
		return 0;

	new_limit.rlim_cur = needed;

	if (fd_limit.rlim_max < needed)
		/* Try to bump hard limit (root only) */
		new_limit.rlim_max = needed;
	else
		new_limit.rlim_max = fd_limit.rlim_max;

	if (setrlimit(RLIMIT_NOFILE, &new_limit)) {
		perror("setrlimit(RLIMIT_NOFILE)");
		pr_warning("not enough FDs for full MSI-X support (estimated need: %zu)",
			   (size_t)(needed - fd_limit.rlim_cur));
	}

	return 0;
}

static int vfio_pci_init_msis(struct kvm *kvm, struct vfio_device *vdev,
			     struct vfio_pci_msi_common *msis)
{
	int ret;
	size_t i;
	int *eventfds;
	size_t irq_set_size;
	struct vfio_pci_msi_entry *entry;
	size_t nr_entries = msis->nr_entries;

	ret = ioctl(vdev->fd, VFIO_DEVICE_GET_IRQ_INFO, &msis->info);
	if (ret || msis->info.count == 0) {
		vfio_dev_err(vdev, "no MSI reported by VFIO");
		return -ENODEV;
	}

	if (!(msis->info.flags & VFIO_IRQ_INFO_EVENTFD)) {
		vfio_dev_err(vdev, "interrupt not EVENTFD capable");
		return -EINVAL;
	}

	if (msis->info.count != nr_entries) {
		vfio_dev_err(vdev, "invalid number of MSIs reported by VFIO");
		return -EINVAL;
	}

	mutex_init(&msis->mutex);

	vfio_pci_reserve_irq_fds(nr_entries);

	irq_set_size = sizeof(struct vfio_irq_set) + nr_entries * sizeof(int);
	msis->irq_set = malloc(irq_set_size);
	if (!msis->irq_set)
		return -ENOMEM;

	*msis->irq_set = (struct vfio_irq_set) {
		.argsz	= irq_set_size,
		.flags 	= VFIO_IRQ_SET_DATA_EVENTFD |
			  VFIO_IRQ_SET_ACTION_TRIGGER,
		.index 	= msis->info.index,
		.start 	= 0,
		.count 	= nr_entries,
	};

	eventfds = (void *)msis->irq_set + sizeof(struct vfio_irq_set);

	for (i = 0; i < nr_entries; i++) {
		entry = &msis->entries[i];
		entry->gsi = -1;
		entry->eventfd = -1;
		msi_set_masked(entry->virt_state, true);
		msi_set_masked(entry->phys_state, true);
		eventfds[i] = -1;
	}

	return 0;
}

static void vfio_pci_disable_intx(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_pci_device *pdev = &vdev->pci;
	int gsi = pdev->intx_gsi;
	struct vfio_irq_set irq_set = {
		.argsz	= sizeof(irq_set),
		.flags	= VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index	= VFIO_PCI_INTX_IRQ_INDEX,
	};

	if (pdev->intx_fd == -1)
		return;

	pr_debug("user requested MSI, disabling INTx %d", gsi);

	ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);
	irq__del_irqfd(kvm, gsi, pdev->intx_fd);

	close(pdev->intx_fd);
	close(pdev->unmask_fd);
	pdev->intx_fd = -1;
}

static int vfio_pci_enable_intx(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	int trigger_fd, unmask_fd;
	union vfio_irq_eventfd	trigger;
	union vfio_irq_eventfd	unmask;
	struct vfio_pci_device *pdev = &vdev->pci;
	int gsi = pdev->intx_gsi;

	if (pdev->intx_fd != -1)
		return 0;

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
	set_vfio_irq_eventd_payload(&trigger, trigger_fd);

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
	set_vfio_irq_eventd_payload(&unmask, unmask_fd);

	ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
	if (ret < 0) {
		vfio_dev_err(vdev, "failed to setup unmask IRQ");
		goto err_remove_event;
	}

	pdev->intx_fd = trigger_fd;
	pdev->unmask_fd = unmask_fd;

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

static int vfio_pci_init_intx(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	struct vfio_pci_device *pdev = &vdev->pci;
	struct vfio_irq_info irq_info = {
		.argsz = sizeof(irq_info),
		.index = VFIO_PCI_INTX_IRQ_INDEX,
	};

	vfio_pci_reserve_irq_fds(2);

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

	/* Guest is going to ovewrite our irq_line... */
	pdev->intx_gsi = pdev->hdr.irq_line - KVM_IRQ_OFFSET;

	pdev->intx_fd = -1;

	return 0;
}

static int vfio_pci_configure_dev_irqs(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret = 0;
	struct vfio_pci_device *pdev = &vdev->pci;

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
		pdev->msix.info = (struct vfio_irq_info) {
			.argsz = sizeof(pdev->msix.info),
			.index = VFIO_PCI_MSIX_IRQ_INDEX,
		};
		ret = vfio_pci_init_msis(kvm, vdev, &pdev->msix);
		if (ret)
			return ret;
	}

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI) {
		pdev->msi.info = (struct vfio_irq_info) {
			.argsz = sizeof(pdev->msi.info),
			.index = VFIO_PCI_MSI_IRQ_INDEX,
		};
		ret = vfio_pci_init_msis(kvm, vdev, &pdev->msi);
		if (ret)
			return ret;
	}

	if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_INTX) {
		ret = vfio_pci_init_intx(kvm, vdev);
		if (ret)
			return ret;

		ret = vfio_pci_enable_intx(kvm, vdev);
	}

	return ret;
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
	struct vfio_pci_device *pdev = &vdev->pci;

	for (i = 0; i < vdev->info.num_regions; i++)
		vfio_unmap_region(kvm, &vdev->regions[i]);

	device__unregister(&vdev->dev_hdr);

	free(pdev->msix.irq_set);
	free(pdev->msix.entries);
	free(pdev->msi.irq_set);
	free(pdev->msi.entries);
}
