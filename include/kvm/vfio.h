#ifndef KVM__VFIO_H
#define KVM__VFIO_H

#include "kvm/mutex.h"
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

/* MSI/MSI-X capability enabled */
#define VFIO_PCI_MSI_STATE_ENABLED	(1 << 0)
/* MSI/MSI-X capability or individual vector masked */
#define VFIO_PCI_MSI_STATE_MASKED	(1 << 1)
/* MSI-X capability has no vector enabled yet */
#define VFIO_PCI_MSI_STATE_EMPTY	(1 << 2)

struct vfio_pci_msi_entry {
	struct msix_table		config;
	int				gsi;
	int				eventfd;
	u8				phys_state;
	u8				virt_state;
};

struct vfio_pci_msix_table {
	size_t				size;
	unsigned int			bar;
	u32				guest_phys_addr;
};

struct vfio_pci_msix_pba {
	size_t				size;
	off_t				offset; /* in VFIO device fd */
	unsigned int			bar;
	u32				guest_phys_addr;
};

/* Common data for MSI and MSI-X */
struct vfio_pci_msi_common {
	off_t				pos;
	u8				virt_state;
	u8				phys_state;
	struct mutex			mutex;
	struct vfio_irq_info		info;
	struct vfio_irq_set		*irq_set;
	size_t				nr_entries;
	struct vfio_pci_msi_entry	*entries;
};

#define VFIO_PCI_IRQ_MODE_INTX		(1 << 0)
#define VFIO_PCI_IRQ_MODE_MSI		(1 << 1)
#define VFIO_PCI_IRQ_MODE_MSIX		(1 << 2)

struct vfio_pci_device {
	struct pci_device_header	hdr;

	unsigned long			irq_modes;
	int				intx_fd;
	int				unmask_fd;
	unsigned int			intx_gsi;
	struct vfio_pci_msi_common	msi;
	struct vfio_pci_msi_common	msix;
	struct vfio_pci_msix_table	msix_table;
	struct vfio_pci_msix_pba	msix_pba;
};

struct vfio_region {
	struct vfio_region_info		info;
	struct vfio_device		*vdev;
	u64				guest_phys_addr;
	void				*host_addr;
	u32				port_base;
	int				is_ioport	:1;
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
