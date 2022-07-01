#ifndef KVM__VIRTIO_MMIO_H
#define KVM__VIRTIO_MMIO_H

#include <linux/types.h>
#include <linux/virtio_mmio.h>

#include <kvm/kvm-cpu.h>

#define VIRTIO_MMIO_MAX_VQ	32
#define VIRTIO_MMIO_MAX_CONFIG	1
#define VIRTIO_MMIO_IO_SIZE	0x200

struct kvm;

struct virtio_mmio_ioevent_param {
	struct virtio_device	*vdev;
	u32			vq;
};

struct virtio_mmio_hdr {
	char	magic[4];
	u32	version;
	u32	device_id;
	u32	vendor_id;
	u32	host_features;
	u32	host_features_sel;
	u32	reserved_1[2];
	u32	guest_features;
	u32	guest_features_sel;
	u32	guest_page_size;	/* legacy */
	u32	reserved_2;
	u32	queue_sel;
	u32	queue_num_max;
	u32	queue_num;
	u32	queue_align;		/* legacy */
	u32	queue_pfn;		/* legacy */
	u32	queue_ready;		/* modern */
	u32	reserved_3[2];
	u32	queue_notify;
	u32	reserved_4[3];
	u32	interrupt_state;
	u32	interrupt_ack;
	u32	reserved_5[2];
	u32	status;
	u32	reserved_7[3];
	u32	queue_desc_low;		/* modern */
	u32	queue_desc_high;
	u32	reserved_8[2];
	u32	queue_avail_low;
	u32	queue_avail_high;
	u32	reserved_9[2];
	u32	queue_used_low;
	u32	queue_used_high;
} __attribute__((packed));

struct virtio_mmio {
	u32			addr;
	void			*dev;
	struct kvm		*kvm;
	u8			irq;
	struct virtio_mmio_hdr	hdr;
	struct device_header	dev_hdr;
	struct virtio_mmio_ioevent_param ioeventfds[VIRTIO_MMIO_MAX_VQ];
};

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq);
int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev);
int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev);
int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev);
int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		      int device_id, int subsys_id, int class);
int virtio_mmio_init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev,
			       u32 vq);

void virtio_mmio_legacy_callback(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				 u32 len, u8 is_write, void *ptr);
void virtio_mmio_modern_callback(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				 u32 len, u8 is_write, void *ptr);
int virtio_mmio_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq);
void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq);
#endif
