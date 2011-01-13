#ifndef KVM__IOPORT_H
#define KVM__IOPORT_H

#include <stdbool.h>
#include <stdint.h>

/* some ports we reserve for own use */
#define IOPORT_DBG		0xe0
#define IOPORT_VIRTIO_BLK	0xc200	/* Virtio block device */

struct kvm;

struct ioport_operations {
	bool (*io_in)(struct kvm *self, uint16_t port, void *data, int size, uint32_t count);
	bool (*io_out)(struct kvm *self, uint16_t port, void *data, int size, uint32_t count);
};

void ioport__register(uint16_t port, struct ioport_operations *ops, int count);

static inline uint8_t ioport__read8(uint8_t *data)
{
	return *data;
}

static inline uint16_t ioport__read16(uint16_t *data)
{
	return *data;
}

static inline uint32_t ioport__read32(uint32_t *data)
{
	return *data;
}

static inline void ioport__write8(uint8_t *data, uint8_t value)
{
	*data		 = value;
}

static inline void ioport__write16(uint16_t *data, uint16_t value)
{
	*data		 = value;
}

static inline void ioport__write32(uint32_t *data, uint32_t value)
{
	*data		 = value;
}

#endif /* KVM__IOPORT_H */
