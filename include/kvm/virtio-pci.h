#ifndef KVM__VIRTIO_PCI_H
#define KVM__VIRTIO_PCI_H

#include "kvm/devices.h"
#include "kvm/pci.h"
#include "kvm/virtio.h"

#include <stdbool.h>
#include <linux/byteorder.h>
#include <linux/types.h>

#define VIRTIO_PCI_MAX_VQ	32
#define VIRTIO_PCI_MAX_CONFIG	1

struct kvm;
struct kvm_cpu;

struct virtio_pci_ioevent_param {
	struct virtio_device	*vdev;
	u32			vq;
};

#define ALIGN_UP(x, s)		ALIGN((x) + (s) - 1, (s))
#define VIRTIO_NR_MSIX		(VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG)
#define VIRTIO_MSIX_TABLE_SIZE	(VIRTIO_NR_MSIX * 16)
#define VIRTIO_MSIX_PBA_SIZE	(ALIGN_UP(VIRTIO_MSIX_TABLE_SIZE, 64) / 8)
#define VIRTIO_MSIX_BAR_SIZE	(1UL << fls_long(VIRTIO_MSIX_TABLE_SIZE + \
						 VIRTIO_MSIX_PBA_SIZE))

struct virtio_pci {
	struct pci_device_header pci_hdr;
	struct device_header	dev_hdr;
	void			*dev;
	struct kvm		*kvm;

	u32			doorbell_offset;
	bool			signal_msi;
	u8			status;
	u8			isr;
	u32			device_features_sel;
	u32			driver_features_sel;

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
	u16			vq_vector[VIRTIO_PCI_MAX_VQ];
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
int virtio_pci_modern_init(struct virtio_device *vdev);

static inline bool virtio_pci__msix_enabled(struct virtio_pci *vpci)
{
	return vpci->pci_hdr.msix.ctrl & cpu_to_le16(PCI_MSIX_FLAGS_ENABLE);
}

static inline u16 virtio_pci__port_addr(struct virtio_pci *vpci)
{
	return pci__bar_address(&vpci->pci_hdr, 0);
}

static inline u32 virtio_pci__mmio_addr(struct virtio_pci *vpci)
{
	return pci__bar_address(&vpci->pci_hdr, 1);
}

static inline u32 virtio_pci__msix_io_addr(struct virtio_pci *vpci)
{
	return pci__bar_address(&vpci->pci_hdr, 2);
}

int virtio_pci__add_msix_route(struct virtio_pci *vpci, u32 vec);
int virtio_pci__init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev,
			       u32 vq);
int virtio_pci_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq);
void virtio_pci_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq);

void virtio_pci_legacy__io_mmio_callback(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				  u32 len, u8 is_write, void *ptr);
void virtio_pci_modern__io_mmio_callback(struct kvm_cpu *vcpu, u64 addr, u8 *data,
					 u32 len, u8 is_write, void *ptr);

#endif
