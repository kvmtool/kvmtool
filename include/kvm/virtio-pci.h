#ifndef KVM__VIRTIO_PCI_H
#define KVM__VIRTIO_PCI_H

#include "kvm/devices.h"
#include "kvm/pci.h"

#include <linux/types.h>

#define VIRTIO_PCI_MAX_VQ	32
#define VIRTIO_PCI_MAX_CONFIG	1

struct kvm;

struct virtio_pci_ioevent_param {
	struct virtio_device	*vdev;
	u32			vq;
};

#define VIRTIO_PCI_F_SIGNAL_MSI (1 << 0)

struct virtio_pci {
	struct pci_device_header pci_hdr;
	struct device_header	dev_hdr;
	void			*dev;
	struct kvm		*kvm;

	u8			status;
	u8			isr;
	u32			features;

	/*
	 * We cannot rely on the INTERRUPT_LINE byte in the config space once
	 * we have run guest code, as the OS is allowed to use that field
	 * as a scratch pad to communicate between driver and PCI layer.
	 * So store our legacy interrupt line number in here for internal use.
	 */
	u8			legacy_irq_line;

	/* MSI-X */
	u16			config_vector;
	u32			config_gsi;
	u32			vq_vector[VIRTIO_PCI_MAX_VQ];
	u32			gsis[VIRTIO_PCI_MAX_VQ];
	u64			msix_pba;
	struct msix_table	msix_table[VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG];

	/* virtio queue */
	u16			queue_selector;
	struct virtio_pci_ioevent_param ioeventfds[VIRTIO_PCI_MAX_VQ];
};

int virtio_pci__signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq);
int virtio_pci__signal_config(struct kvm *kvm, struct virtio_device *vdev);
int virtio_pci__exit(struct kvm *kvm, struct virtio_device *vdev);
int virtio_pci__reset(struct kvm *kvm, struct virtio_device *vdev);
int virtio_pci__init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class);

#endif
