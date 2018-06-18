#ifndef KVM__VFIO_H
#define KVM__VFIO_H

#include "kvm/parse-options.h"
#include "kvm/pci.h"

#include <linux/vfio.h>

#define vfio_dev_err(vdev, fmt, ...) \
	pr_err("%s: " fmt, (vdev)->params->name, ##__VA_ARGS__)
#define vfio_dev_warn(vdev, fmt, ...) \
	pr_warning("%s: " fmt, (vdev)->params->name, ##__VA_ARGS__)
#define vfio_dev_info(vdev, fmt, ...) \
	pr_info("%s: " fmt, (vdev)->params->name, ##__VA_ARGS__)
#define vfio_dev_dbg(vdev, fmt, ...) \
	pr_debug("%s: " fmt, (vdev)->params->name, ##__VA_ARGS__)
#define vfio_dev_die(vdev, fmt, ...) \
	die("%s: " fmt, (vdev)->params->name, ##__VA_ARGS__)

/* Currently limited by num_vfio_devices */
#define MAX_VFIO_DEVICES		256

enum vfio_device_type {
	VFIO_DEVICE_PCI,
};

struct vfio_pci_device {
	struct pci_device_header	hdr;
};

struct vfio_region {
	struct vfio_region_info		info;
	u64				guest_phys_addr;
	void				*host_addr;
};

struct vfio_device {
	struct device_header		dev_hdr;
	struct vfio_device_params	*params;
	struct vfio_group		*group;

	int				fd;
	struct vfio_device_info		info;
	struct vfio_region		*regions;

	char				*sysfs_path;

	struct vfio_pci_device		pci;
};

struct vfio_device_params {
	char				*name;
	const char			*bus;
	enum vfio_device_type		type;
};

struct vfio_group {
	unsigned long			id; /* iommu_group number in sysfs */
	int				fd;
	int				refs;
	struct list_head		list;
};

int vfio_device_parser(const struct option *opt, const char *arg, int unset);
int vfio_map_region(struct kvm *kvm, struct vfio_device *vdev,
		    struct vfio_region *region);
void vfio_unmap_region(struct kvm *kvm, struct vfio_region *region);
int vfio_pci_setup_device(struct kvm *kvm, struct vfio_device *device);
void vfio_pci_teardown_device(struct kvm *kvm, struct vfio_device *vdev);

#endif /* KVM__VFIO_H */
