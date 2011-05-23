#ifndef KVM__IOPORT_H
#define KVM__IOPORT_H

#include <stdbool.h>
#include <asm/types.h>
#include <linux/types.h>

/* some ports we reserve for own use */
#define IOPORT_DBG			0xe0
#define IOPORT_VESA			0xa200
#define IOPORT_VESA_SIZE		256
#define IOPORT_VIRTIO_P9		0xb200	/* Virtio 9P device */
#define IOPORT_VIRTIO_P9_SIZE		256
#define IOPORT_VIRTIO_BLK		0xc200	/* Virtio block device */
#define IOPORT_VIRTIO_BLK_SIZE		0x200
#define IOPORT_VIRTIO_CONSOLE		0xd200	/* Virtio console device */
#define IOPORT_VIRTIO_CONSOLE_SIZE	256
#define IOPORT_VIRTIO_NET		0xe200	/* Virtio network device */
#define IOPORT_VIRTIO_NET_SIZE		256
#define IOPORT_VIRTIO_RNG		0xf200	/* Virtio network device */
#define IOPORT_VIRTIO_RNG_SIZE		256

struct kvm;

struct ioport_operations {
	bool (*io_in)(struct kvm *kvm, u16 port, void *data, int size, u32 count);
	bool (*io_out)(struct kvm *kvm, u16 port, void *data, int size, u32 count);
};

void ioport__setup_legacy(void);

void ioport__register(u16 port, struct ioport_operations *ops, int count);

static inline u8 ioport__read8(u8 *data)
{
	return *data;
}

static inline u16 ioport__read16(u16 *data)
{
	return *data;
}

static inline u32 ioport__read32(u32 *data)
{
	return *data;
}

static inline void ioport__write8(u8 *data, u8 value)
{
	*data		 = value;
}

static inline void ioport__write16(u16 *data, u16 value)
{
	*data		 = value;
}

static inline void ioport__write32(u32 *data, u32 value)
{
	*data		 = value;
}

#endif /* KVM__IOPORT_H */
