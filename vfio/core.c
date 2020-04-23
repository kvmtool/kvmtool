#include "kvm/kvm.h"
#include "kvm/vfio.h"
#include "kvm/ioport.h"

#include <linux/list.h>

#define VFIO_DEV_DIR		"/dev/vfio"
#define VFIO_DEV_NODE		VFIO_DEV_DIR "/vfio"
#define IOMMU_GROUP_DIR		"/sys/kernel/iommu_groups"

static int vfio_container;
static LIST_HEAD(vfio_groups);
static struct vfio_device *vfio_devices;

static int vfio_device_pci_parser(const struct option *opt, char *arg,
				  struct vfio_device_params *dev)
{
	unsigned int domain, bus, devnr, fn;

	int nr = sscanf(arg, "%4x:%2x:%2x.%1x", &domain, &bus, &devnr, &fn);
	if (nr < 4) {
		domain = 0;
		nr = sscanf(arg, "%2x:%2x.%1x", &bus, &devnr, &fn);
		if (nr < 3) {
			pr_err("Invalid device identifier %s", arg);
			return -EINVAL;
		}
	}

	dev->type = VFIO_DEVICE_PCI;
	dev->bus = "pci";
	dev->name = malloc(13);
	if (!dev->name)
		return -ENOMEM;

	snprintf(dev->name, 13, "%04x:%02x:%02x.%x", domain, bus, devnr, fn);

	return 0;
}

int vfio_device_parser(const struct option *opt, const char *arg, int unset)
{
	int ret = -EINVAL;
	static int idx = 0;
	struct kvm *kvm = opt->ptr;
	struct vfio_device_params *dev, *devs;
	char *cur, *buf = strdup(arg);

	if (!buf)
		return -ENOMEM;

	if (idx >= MAX_VFIO_DEVICES) {
		pr_warning("Too many VFIO devices");
		goto out_free_buf;
	}

	devs = realloc(kvm->cfg.vfio_devices, sizeof(*dev) * (idx + 1));
	if (!devs) {
		ret = -ENOMEM;
		goto out_free_buf;
	}

	kvm->cfg.vfio_devices = devs;
	dev = &devs[idx];

	cur = strtok(buf, ",");
	if (!cur)
		goto out_free_buf;

	if (!strcmp(opt->long_name, "vfio-pci"))
		ret = vfio_device_pci_parser(opt, cur, dev);
	else
		ret = -EINVAL;

	if (!ret)
		kvm->cfg.num_vfio_devices = ++idx;

out_free_buf:
	free(buf);

	return ret;
}

static bool vfio_ioport_in(struct ioport *ioport, struct kvm_cpu *vcpu,
			   u16 port, void *data, int len)
{
	u32 val;
	ssize_t nr;
	struct vfio_region *region = ioport->priv;
	struct vfio_device *vdev = region->vdev;

	u32 offset = port - region->port_base;

	if (!(region->info.flags & VFIO_REGION_INFO_FLAG_READ))
		return false;

	nr = pread(vdev->fd, &val, len, region->info.offset + offset);
	if (nr != len) {
		vfio_dev_err(vdev, "could not read %d bytes from I/O port 0x%x\n",
			     len, port);
		return false;
	}

	switch (len) {
	case 1:
		ioport__write8(data, val);
		break;
	case 2:
		ioport__write16(data, val);
		break;
	case 4:
		ioport__write32(data, val);
		break;
	default:
		return false;
	}

	return true;
}

static bool vfio_ioport_out(struct ioport *ioport, struct kvm_cpu *vcpu,
			    u16 port, void *data, int len)
{
	u32 val;
	ssize_t nr;
	struct vfio_region *region = ioport->priv;
	struct vfio_device *vdev = region->vdev;

	u32 offset = port - region->port_base;

	if (!(region->info.flags & VFIO_REGION_INFO_FLAG_WRITE))
		return false;

	switch (len) {
	case 1:
		val = ioport__read8(data);
		break;
	case 2:
		val = ioport__read16(data);
		break;
	case 4:
		val = ioport__read32(data);
		break;
	default:
		return false;
	}

	nr = pwrite(vdev->fd, &val, len, region->info.offset + offset);
	if (nr != len)
		vfio_dev_err(vdev, "could not write %d bytes to I/O port 0x%x",
			     len, port);

	return nr == len;
}

static struct ioport_operations vfio_ioport_ops = {
	.io_in	= vfio_ioport_in,
	.io_out	= vfio_ioport_out,
};

static void vfio_mmio_access(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len,
			     u8 is_write, void *ptr)
{
	u64 val;
	ssize_t nr;
	struct vfio_region *region = ptr;
	struct vfio_device *vdev = region->vdev;

	u32 offset = addr - region->guest_phys_addr;

	if (len < 1 || len > 8)
		goto err_report;

	if (is_write) {
		if (!(region->info.flags & VFIO_REGION_INFO_FLAG_WRITE))
			goto err_report;

		memcpy(&val, data, len);

		nr = pwrite(vdev->fd, &val, len, region->info.offset + offset);
		if ((u32)nr != len)
			goto err_report;
	} else {
		if (!(region->info.flags & VFIO_REGION_INFO_FLAG_READ))
			goto err_report;

		nr = pread(vdev->fd, &val, len, region->info.offset + offset);
		if ((u32)nr != len)
			goto err_report;

		memcpy(data, &val, len);
	}

	return;

err_report:
	vfio_dev_err(vdev, "could not %s %u bytes at 0x%x (0x%llx)", is_write ?
		     "write" : "read", len, offset, addr);
}

static int vfio_setup_trap_region(struct kvm *kvm, struct vfio_device *vdev,
				  struct vfio_region *region)
{
	if (region->is_ioport) {
		int port = pci_get_io_port_block(region->info.size);

		port = ioport__register(kvm, port, &vfio_ioport_ops,
					region->info.size, region);
		if (port < 0)
			return port;

		region->port_base = port;
		return 0;
	}

	return kvm__register_mmio(kvm, region->guest_phys_addr,
				  region->info.size, false, vfio_mmio_access,
				  region);
}

int vfio_map_region(struct kvm *kvm, struct vfio_device *vdev,
		    struct vfio_region *region)
{
	void *base;
	int ret, prot = 0;
	/* KVM needs page-aligned regions */
	u64 map_size = ALIGN(region->info.size, PAGE_SIZE);

	if (!(region->info.flags & VFIO_REGION_INFO_FLAG_MMAP))
		return vfio_setup_trap_region(kvm, vdev, region);

	if (region->info.flags & VFIO_REGION_INFO_FLAG_READ)
		prot |= PROT_READ;
	if (region->info.flags & VFIO_REGION_INFO_FLAG_WRITE)
		prot |= PROT_WRITE;

	base = mmap(NULL, region->info.size, prot, MAP_SHARED, vdev->fd,
		    region->info.offset);
	if (base == MAP_FAILED) {
		/* TODO: support sparse mmap */
		vfio_dev_warn(vdev, "failed to mmap region %u (0x%llx bytes), falling back to trapping",
			 region->info.index, region->info.size);
		return vfio_setup_trap_region(kvm, vdev, region);
	}
	region->host_addr = base;

	ret = kvm__register_dev_mem(kvm, region->guest_phys_addr, map_size,
				    region->host_addr);
	if (ret) {
		vfio_dev_err(vdev, "failed to register region with KVM");
		return ret;
	}

	return 0;
}

void vfio_unmap_region(struct kvm *kvm, struct vfio_region *region)
{
	u64 map_size;

	if (region->host_addr) {
		map_size = ALIGN(region->info.size, PAGE_SIZE);
		kvm__destroy_mem(kvm, region->guest_phys_addr, map_size,
				 region->host_addr);
		munmap(region->host_addr, region->info.size);
		region->host_addr = NULL;
	} else if (region->is_ioport) {
		ioport__unregister(kvm, region->port_base);
	} else {
		kvm__deregister_mmio(kvm, region->guest_phys_addr);
	}
}

static int vfio_configure_device(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	struct vfio_group *group = vdev->group;

	vdev->fd = ioctl(group->fd, VFIO_GROUP_GET_DEVICE_FD,
			 vdev->params->name);
	if (vdev->fd < 0) {
		vfio_dev_warn(vdev, "failed to get fd");

		/* The device might be a bridge without an fd */
		return 0;
	}

	vdev->info.argsz = sizeof(vdev->info);
	if (ioctl(vdev->fd, VFIO_DEVICE_GET_INFO, &vdev->info)) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to get info");
		goto err_close_device;
	}

	if (vdev->info.flags & VFIO_DEVICE_FLAGS_RESET &&
	    ioctl(vdev->fd, VFIO_DEVICE_RESET) < 0)
		vfio_dev_warn(vdev, "failed to reset device");

	vdev->regions = calloc(vdev->info.num_regions, sizeof(*vdev->regions));
	if (!vdev->regions) {
		ret = -ENOMEM;
		goto err_close_device;
	}

	/* Now for the bus-specific initialization... */
	switch (vdev->params->type) {
	case VFIO_DEVICE_PCI:
		BUG_ON(!(vdev->info.flags & VFIO_DEVICE_FLAGS_PCI));
		ret = vfio_pci_setup_device(kvm, vdev);
		break;
	default:
		BUG_ON(1);
		ret = -EINVAL;
	}

	if (ret)
		goto err_free_regions;

	vfio_dev_info(vdev, "assigned to device number 0x%x in group %lu",
		      vdev->dev_hdr.dev_num, group->id);

	return 0;

err_free_regions:
	free(vdev->regions);
err_close_device:
	close(vdev->fd);

	return ret;
}

static int vfio_configure_devices(struct kvm *kvm)
{
	int i, ret;

	for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
		ret = vfio_configure_device(kvm, &vfio_devices[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int vfio_get_iommu_type(void)
{
	if (ioctl(vfio_container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU))
		return VFIO_TYPE1v2_IOMMU;

	if (ioctl(vfio_container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU))
		return VFIO_TYPE1_IOMMU;

	return -ENODEV;
}

static int vfio_map_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank, void *data)
{
	int ret = 0;
	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz	= sizeof(dma_map),
		.flags	= VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr	= (unsigned long)bank->host_addr,
		.iova	= (u64)bank->guest_phys_addr,
		.size	= bank->size,
	};

	/* Map the guest memory for DMA (i.e. provide isolation) */
	if (ioctl(vfio_container, VFIO_IOMMU_MAP_DMA, &dma_map)) {
		ret = -errno;
		pr_err("Failed to map 0x%llx -> 0x%llx (%llu) for DMA",
		       dma_map.iova, dma_map.vaddr, dma_map.size);
	}

	return ret;
}

static int vfio_unmap_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank, void *data)
{
	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.size = bank->size,
		.iova = bank->guest_phys_addr,
	};

	ioctl(vfio_container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);

	return 0;
}

static int vfio_configure_reserved_regions(struct kvm *kvm,
					   struct vfio_group *group)
{
	FILE *file;
	int ret = 0;
	char type[9];
	char filename[PATH_MAX];
	unsigned long long start, end;

	snprintf(filename, PATH_MAX, IOMMU_GROUP_DIR "/%lu/reserved_regions",
		 group->id);

	/* reserved_regions might not be present on older systems */
	if (access(filename, F_OK))
		return 0;

	file = fopen(filename, "r");
	if (!file)
		return -errno;

	while (fscanf(file, "0x%llx 0x%llx %8s\n", &start, &end, type) == 3) {
		ret = kvm__reserve_mem(kvm, start, end - start + 1);
		if (ret)
			break;
	}

	fclose(file);

	return ret;
}

static int vfio_configure_groups(struct kvm *kvm)
{
	int ret;
	struct vfio_group *group;

	list_for_each_entry(group, &vfio_groups, list) {
		ret = vfio_configure_reserved_regions(kvm, group);
		if (ret)
			return ret;
	}

	return 0;
}

static struct vfio_group *vfio_group_create(struct kvm *kvm, unsigned long id)
{
	int ret;
	struct vfio_group *group;
	char group_node[PATH_MAX];
	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status),
	};

	group = calloc(1, sizeof(*group));
	if (!group)
		return NULL;

	group->id	= id;
	group->refs	= 1;

	ret = snprintf(group_node, PATH_MAX, VFIO_DEV_DIR "/%lu", id);
	if (ret < 0 || ret == PATH_MAX)
		return NULL;

	group->fd = open(group_node, O_RDWR);
	if (group->fd < 0) {
		pr_err("Failed to open IOMMU group %s", group_node);
		goto err_free_group;
	}

	if (ioctl(group->fd, VFIO_GROUP_GET_STATUS, &group_status)) {
		pr_err("Failed to determine status of IOMMU group %lu", id);
		goto err_close_group;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		pr_err("IOMMU group %lu is not viable", id);
		goto err_close_group;
	}

	if (ioctl(group->fd, VFIO_GROUP_SET_CONTAINER, &vfio_container)) {
		pr_err("Failed to add IOMMU group %lu to VFIO container", id);
		goto err_close_group;
	}

	list_add(&group->list, &vfio_groups);

	return group;

err_close_group:
	close(group->fd);
err_free_group:
	free(group);

	return NULL;
}

static void vfio_group_exit(struct kvm *kvm, struct vfio_group *group)
{
	if (--group->refs != 0)
		return;

	ioctl(group->fd, VFIO_GROUP_UNSET_CONTAINER);

	list_del(&group->list);
	close(group->fd);
	free(group);
}

static struct vfio_group *
vfio_group_get_for_dev(struct kvm *kvm, struct vfio_device *vdev)
{
	int dirfd;
	ssize_t ret;
	char *group_name;
	unsigned long group_id;
	char group_path[PATH_MAX];
	struct vfio_group *group = NULL;

	/* Find IOMMU group for this device */
	dirfd = open(vdev->sysfs_path, O_DIRECTORY | O_PATH | O_RDONLY);
	if (dirfd < 0) {
		vfio_dev_err(vdev, "failed to open '%s'", vdev->sysfs_path);
		return NULL;
	}

	ret = readlinkat(dirfd, "iommu_group", group_path, PATH_MAX);
	if (ret < 0) {
		vfio_dev_err(vdev, "no iommu_group");
		goto out_close;
	}
	if (ret == PATH_MAX)
		goto out_close;

	group_path[ret] = '\0';

	group_name = basename(group_path);
	errno = 0;
	group_id = strtoul(group_name, NULL, 10);
	if (errno)
		goto out_close;

	list_for_each_entry(group, &vfio_groups, list) {
		if (group->id == group_id) {
			group->refs++;
			return group;
		}
	}

	group = vfio_group_create(kvm, group_id);

out_close:
	close(dirfd);
	return group;
}

static int vfio_device_init(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	char dev_path[PATH_MAX];
	struct vfio_group *group;

	ret = snprintf(dev_path, PATH_MAX, "/sys/bus/%s/devices/%s",
		       vdev->params->bus, vdev->params->name);
	if (ret < 0 || ret == PATH_MAX)
		return -EINVAL;

	vdev->sysfs_path = strndup(dev_path, PATH_MAX);
	if (!vdev->sysfs_path)
		return -errno;

	group = vfio_group_get_for_dev(kvm, vdev);
	if (!group) {
		free(vdev->sysfs_path);
		return -EINVAL;
	}

	vdev->group = group;

	return 0;
}

static void vfio_device_exit(struct kvm *kvm, struct vfio_device *vdev)
{
	vfio_group_exit(kvm, vdev->group);

	switch (vdev->params->type) {
	case VFIO_DEVICE_PCI:
		vfio_pci_teardown_device(kvm, vdev);
		break;
	default:
		vfio_dev_warn(vdev, "no teardown function for device");
	}

	close(vdev->fd);

	free(vdev->regions);
	free(vdev->sysfs_path);
}

static int vfio_container_init(struct kvm *kvm)
{
	int api, i, ret, iommu_type;;

	/* Create a container for our IOMMU groups */
	vfio_container = open(VFIO_DEV_NODE, O_RDWR);
	if (vfio_container == -1) {
		ret = errno;
		pr_err("Failed to open %s", VFIO_DEV_NODE);
		return ret;
	}

	api = ioctl(vfio_container, VFIO_GET_API_VERSION);
	if (api != VFIO_API_VERSION) {
		pr_err("Unknown VFIO API version %d", api);
		return -ENODEV;
	}

	iommu_type = vfio_get_iommu_type();
	if (iommu_type < 0) {
		pr_err("VFIO type-1 IOMMU not supported on this platform");
		return iommu_type;
	}

	/* Create groups for our devices and add them to the container */
	for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
		vfio_devices[i].params = &kvm->cfg.vfio_devices[i];

		ret = vfio_device_init(kvm, &vfio_devices[i]);
		if (ret)
			return ret;
	}

	/* Finalise the container */
	if (ioctl(vfio_container, VFIO_SET_IOMMU, iommu_type)) {
		ret = -errno;
		pr_err("Failed to set IOMMU type %d for VFIO container",
		       iommu_type);
		return ret;
	} else {
		pr_info("Using IOMMU type %d for VFIO container", iommu_type);
	}

	return kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM, vfio_map_mem_bank,
				      NULL);
}

static int vfio__init(struct kvm *kvm)
{
	int ret;

	if (!kvm->cfg.num_vfio_devices)
		return 0;

	vfio_devices = calloc(kvm->cfg.num_vfio_devices, sizeof(*vfio_devices));
	if (!vfio_devices)
		return -ENOMEM;

	ret = vfio_container_init(kvm);
	if (ret)
		return ret;

	ret = vfio_configure_groups(kvm);
	if (ret)
		return ret;

	ret = vfio_configure_devices(kvm);
	if (ret)
		return ret;

	return 0;
}
dev_base_init(vfio__init);

static int vfio__exit(struct kvm *kvm)
{
	int i;

	if (!kvm->cfg.num_vfio_devices)
		return 0;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++)
		vfio_device_exit(kvm, &vfio_devices[i]);

	free(vfio_devices);

	kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM, vfio_unmap_mem_bank, NULL);
	close(vfio_container);

	free(kvm->cfg.vfio_devices);

	return 0;
}
dev_base_exit(vfio__exit);
