#ifndef KVM__VIRTIO_TRANS_H
#define KVM__VIRTIO_TRANS_H

#include "kvm/kvm.h"

enum virtio_trans_type {
	VIRTIO_PCI,
};

struct virtio_trans;

struct virtio_ops {
	void (*set_config)(struct kvm *kvm, void *dev, u8 data, u32 offset);
	u8 (*get_config)(struct kvm *kvm, void *dev, u32 offset);

	u32 (*get_host_features)(struct kvm *kvm, void *dev);
	void (*set_guest_features)(struct kvm *kvm, void *dev, u32 features);

	int (*init_vq)(struct kvm *kvm, void *dev, u32 vq, u32 pfn);
	int (*notify_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_pfn_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_size_vq)(struct kvm *kvm, void *dev, u32 vq);
	void (*notify_vq_gsi)(struct kvm *kvm, void *dev, u32 vq, u32 gsi);
	void (*notify_vq_eventfd)(struct kvm *kvm, void *dev, u32 vq, u32 efd);
};

struct virtio_trans_ops {
	int (*init)(struct kvm *kvm, struct virtio_trans *vtrans, void *dev, int device_id,
			int subsys_id, int class);
	int (*uninit)(struct kvm *kvm, struct virtio_trans *vtrans);
	int (*signal_vq)(struct kvm *kvm, struct virtio_trans *virtio_trans, u32 queueid);
	int (*signal_config)(struct kvm *kvm, struct virtio_trans *virtio_trans);
};

struct virtio_trans {
	void			*virtio;
	enum virtio_trans_type	type;
	struct virtio_trans_ops	*trans_ops;
	struct virtio_ops	*virtio_ops;
};

int virtio_trans_init(struct virtio_trans *vtrans, enum virtio_trans_type type);

#endif