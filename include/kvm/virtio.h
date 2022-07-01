#ifndef KVM__VIRTIO_H
#define KVM__VIRTIO_H

#include <endian.h>

#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/virtio_config.h>
#include <sys/uio.h>

#include "kvm/barrier.h"
#include "kvm/kvm.h"

#define VIRTIO_IRQ_LOW		0
#define VIRTIO_IRQ_HIGH		1

#define VIRTIO_PCI_O_CONFIG	0
#define VIRTIO_PCI_O_MSIX	1

#define VIRTIO_ENDIAN_LE	(1 << 0)
#define VIRTIO_ENDIAN_BE	(1 << 1)

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define VIRTIO_ENDIAN_HOST VIRTIO_ENDIAN_LE
#else
#define VIRTIO_ENDIAN_HOST VIRTIO_ENDIAN_BE
#endif

/* Reserved status bits */
#define VIRTIO_CONFIG_S_MASK \
	(VIRTIO_CONFIG_S_ACKNOWLEDGE |	\
	 VIRTIO_CONFIG_S_DRIVER |	\
	 VIRTIO_CONFIG_S_DRIVER_OK |	\
	 VIRTIO_CONFIG_S_FEATURES_OK |	\
	 VIRTIO_CONFIG_S_NEEDS_RESET |	\
	 VIRTIO_CONFIG_S_FAILED)

/* Kvmtool status bits */
/* Start the device */
#define VIRTIO__STATUS_START		(1 << 8)
/* Stop the device */
#define VIRTIO__STATUS_STOP		(1 << 9)
/* Initialize the config */
#define VIRTIO__STATUS_CONFIG		(1 << 10)

struct vring_addr {
	bool			legacy;
	union {
		/* Legacy description */
		struct {
			u32	pfn;
			u32	align;
			u32	pgsize;
		};
		/* Modern description */
		struct {
			u32	desc_lo;
			u32	desc_hi;
			u32	avail_lo;
			u32	avail_hi;
			u32	used_lo;
			u32	used_hi;
		};
	};
};

struct virt_queue {
	struct vring	vring;
	struct vring_addr vring_addr;
	/* The last_avail_idx field is an index to ->ring of struct vring_avail.
	   It's where we assume the next request index is at.  */
	u16		last_avail_idx;
	u16		last_used_signalled;
	u16		endian;
	bool		use_event_idx;
	bool		enabled;
};

/*
 * The default policy is not to cope with the guest endianness.
 * It also helps not breaking archs that do not care about supporting
 * such a configuration.
 */
#ifndef VIRTIO_RING_ENDIAN
#define VIRTIO_RING_ENDIAN VIRTIO_ENDIAN_HOST
#endif

#if VIRTIO_RING_ENDIAN != VIRTIO_ENDIAN_HOST

static inline __u16 __virtio_g2h_u16(u16 endian, __u16 val)
{
	return (endian == VIRTIO_ENDIAN_LE) ? le16toh(val) : be16toh(val);
}

static inline __u16 __virtio_h2g_u16(u16 endian, __u16 val)
{
	return (endian == VIRTIO_ENDIAN_LE) ? htole16(val) : htobe16(val);
}

static inline __u32 __virtio_g2h_u32(u16 endian, __u32 val)
{
	return (endian == VIRTIO_ENDIAN_LE) ? le32toh(val) : be32toh(val);
}

static inline __u32 __virtio_h2g_u32(u16 endian, __u32 val)
{
	return (endian == VIRTIO_ENDIAN_LE) ? htole32(val) : htobe32(val);
}

static inline __u64 __virtio_g2h_u64(u16 endian, __u64 val)
{
	return (endian == VIRTIO_ENDIAN_LE) ? le64toh(val) : be64toh(val);
}

static inline __u64 __virtio_h2g_u64(u16 endian, __u64 val)
{
	return (endian == VIRTIO_ENDIAN_LE) ? htole64(val) : htobe64(val);
}

#define virtio_guest_to_host_u16(x, v)	__virtio_g2h_u16((x)->endian, (v))
#define virtio_host_to_guest_u16(x, v)	__virtio_h2g_u16((x)->endian, (v))
#define virtio_guest_to_host_u32(x, v)	__virtio_g2h_u32((x)->endian, (v))
#define virtio_host_to_guest_u32(x, v)	__virtio_h2g_u32((x)->endian, (v))
#define virtio_guest_to_host_u64(x, v)	__virtio_g2h_u64((x)->endian, (v))
#define virtio_host_to_guest_u64(x, v)	__virtio_h2g_u64((x)->endian, (v))

#else

#define virtio_guest_to_host_u16(x, v)	(v)
#define virtio_host_to_guest_u16(x, v)	(v)
#define virtio_guest_to_host_u32(x, v)	(v)
#define virtio_host_to_guest_u32(x, v)	(v)
#define virtio_guest_to_host_u64(x, v)	(v)
#define virtio_host_to_guest_u64(x, v)	(v)

#endif

static inline u16 virt_queue__pop(struct virt_queue *queue)
{
	__u16 guest_idx;

	/*
	 * The guest updates the avail index after writing the ring entry.
	 * Ensure that we read the updated entry once virt_queue__available()
	 * observes the new index.
	 */
	rmb();

	guest_idx = queue->vring.avail->ring[queue->last_avail_idx++ % queue->vring.num];
	return virtio_guest_to_host_u16(queue, guest_idx);
}

static inline struct vring_desc *virt_queue__get_desc(struct virt_queue *queue, u16 desc_ndx)
{
	return &queue->vring.desc[desc_ndx];
}

static inline bool virt_queue__available(struct virt_queue *vq)
{
	u16 last_avail_idx = virtio_host_to_guest_u16(vq, vq->last_avail_idx);

	if (!vq->vring.avail)
		return 0;

	if (vq->use_event_idx) {
		vring_avail_event(&vq->vring) = last_avail_idx;
		/*
		 * After the driver writes a new avail index, it reads the event
		 * index to see if we need any notification. Ensure that it
		 * reads the updated index, or else we'll miss the notification.
		 */
		mb();
	}

	return vq->vring.avail->idx != last_avail_idx;
}

void virt_queue__used_idx_advance(struct virt_queue *queue, u16 jump);
struct vring_used_elem * virt_queue__set_used_elem_no_update(struct virt_queue *queue, u32 head, u32 len, u16 offset);
struct vring_used_elem *virt_queue__set_used_elem(struct virt_queue *queue, u32 head, u32 len);

bool virtio_queue__should_signal(struct virt_queue *vq);
u16 virt_queue__get_iov(struct virt_queue *vq, struct iovec iov[],
			u16 *out, u16 *in, struct kvm *kvm);
u16 virt_queue__get_head_iov(struct virt_queue *vq, struct iovec iov[],
			     u16 *out, u16 *in, u16 head, struct kvm *kvm);
u16 virt_queue__get_inout_iov(struct kvm *kvm, struct virt_queue *queue,
			      struct iovec in_iov[], struct iovec out_iov[],
			      u16 *in, u16 *out);
int virtio__get_dev_specific_field(int offset, bool msix, u32 *config_off);

enum virtio_trans {
	VIRTIO_PCI,
	VIRTIO_PCI_LEGACY,
	VIRTIO_MMIO,
	VIRTIO_MMIO_LEGACY,
};

struct virtio_device {
	bool			legacy;
	bool			use_vhost;
	void			*virtio;
	struct virtio_ops	*ops;
	u16			endian;
	u64			features;
	u32			status;
};

struct virtio_ops {
	u8 *(*get_config)(struct kvm *kvm, void *dev);
	size_t (*get_config_size)(struct kvm *kvm, void *dev);
	u64 (*get_host_features)(struct kvm *kvm, void *dev);
	unsigned int (*get_vq_count)(struct kvm *kvm, void *dev);
	int (*init_vq)(struct kvm *kvm, void *dev, u32 vq);
	void (*exit_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*notify_vq)(struct kvm *kvm, void *dev, u32 vq);
	struct virt_queue *(*get_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*get_size_vq)(struct kvm *kvm, void *dev, u32 vq);
	int (*set_size_vq)(struct kvm *kvm, void *dev, u32 vq, int size);
	void (*notify_vq_gsi)(struct kvm *kvm, void *dev, u32 vq, u32 gsi);
	void (*notify_vq_eventfd)(struct kvm *kvm, void *dev, u32 vq, u32 efd);
	int (*signal_vq)(struct kvm *kvm, struct virtio_device *vdev, u32 queueid);
	int (*signal_config)(struct kvm *kvm, struct virtio_device *vdev);
	void (*notify_status)(struct kvm *kvm, void *dev, u32 status);
	int (*init)(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		    int device_id, int subsys_id, int class);
	int (*exit)(struct kvm *kvm, struct virtio_device *vdev);
	int (*reset)(struct kvm *kvm, struct virtio_device *vdev);
};

int __must_check virtio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
			     struct virtio_ops *ops, enum virtio_trans trans,
			     int device_id, int subsys_id, int class);
int virtio_compat_add_message(const char *device, const char *config);
const char* virtio_trans_name(enum virtio_trans trans);
void virtio_init_device_vq(struct kvm *kvm, struct virtio_device *vdev,
			   struct virt_queue *vq, size_t nr_descs);
void virtio_exit_vq(struct kvm *kvm, struct virtio_device *vdev, void *dev,
		    int num);
bool virtio_access_config(struct kvm *kvm, struct virtio_device *vdev, void *dev,
			  unsigned long offset, void *data, size_t size,
			  bool is_write);
void virtio_set_guest_features(struct kvm *kvm, struct virtio_device *vdev,
			       void *dev, u64 features);
void virtio_notify_status(struct kvm *kvm, struct virtio_device *vdev,
			  void *dev, u8 status);

#endif /* KVM__VIRTIO_H */
