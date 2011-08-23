#ifndef KVM__VIRTIO_PCI_H
#define KVM__VIRTIO_PCI_H

#include "kvm/pci.h"

#include <linux/types.h>

#define VIRTIO_PCI_MAX_VQ 3

struct kvm;

struct virtio_pci_ops {
	void (*set_config)(struct kvm *kvm, void *dev, u8 data, u32 offset);
	u8 (*get_config)(struct kvm *kvm, void *dev, u32 offset);

	u32 (*get_host_features)(struct kvm *kvm, void *dev);
	void (*set_guest_features)(struct kvm *kvm, void *dev, u32 features);

	int (*init_vq)(struct kvm *kvm, void *dev, u32 vq, u32 pfn);
	int (*notify_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_pfn_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_size_vq)(struct kvm *kvm, void *dev, u32 vq);
};

struct virtio_pci_ioevent_param {
	struct virtio_pci	*vpci;
	u32			vq;
};

struct virtio_pci {
	struct pci_device_header pci_hdr;
	struct virtio_pci_ops	ops;
	void			*dev;

	u16			base_addr;
	u8			status;
	u8			isr;

	/* MSI-X */
	u16			config_vector;
	u32			config_gsi;
	u32			vq_vector[VIRTIO_PCI_MAX_VQ];
	u32			gsis[VIRTIO_PCI_MAX_VQ];
	u32			msix_io_block;
	u32			msix_pba_block;
	u64			msix_pba;
	struct msix_table	msix_table[VIRTIO_PCI_MAX_VQ + 1];

	/* virtio queue */
	u16			queue_selector;
	struct virtio_pci_ioevent_param ioeventfds[VIRTIO_PCI_MAX_VQ];
};

int virtio_pci__init(struct kvm *kvm, struct virtio_pci *vpci, void *dev,
			int device_id, int subsys_id);
int virtio_pci__signal_vq(struct kvm *kvm, struct virtio_pci *vpci, u32 vq);
int virtio_pci__signal_config(struct kvm *kvm, struct virtio_pci *vpci);

#endif
