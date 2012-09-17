#ifndef KVM__IOPORT_H
#define KVM__IOPORT_H

#include "kvm/rbtree-interval.h"

#include <stdbool.h>
#include <limits.h>
#include <asm/types.h>
#include <linux/types.h>
#include <linux/byteorder.h>

/* some ports we reserve for own use */
#define IOPORT_DBG			0xe0
#define IOPORT_START			0x6200
#define IOPORT_SIZE			0x400

#define IOPORT_EMPTY			USHRT_MAX

struct kvm;

struct ioport {
	struct rb_int_node		node;
	struct ioport_operations	*ops;
	void				*priv;
};

struct ioport_operations {
	bool (*io_in)(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size);
	bool (*io_out)(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size);
};

void ioport__setup_arch(struct kvm *kvm);

int ioport__register(struct kvm *kvm, u16 port, struct ioport_operations *ops,
			int count, void *param);
int ioport__unregister(struct kvm *kvm, u16 port);
int ioport__init(struct kvm *kvm);
int ioport__exit(struct kvm *kvm);

static inline u8 ioport__read8(u8 *data)
{
	return *data;
}
/* On BE platforms, PCI I/O is byteswapped, i.e. LE, so swap back. */
static inline u16 ioport__read16(u16 *data)
{
	return le16_to_cpu(*data);
}

static inline u32 ioport__read32(u32 *data)
{
	return le32_to_cpu(*data);
}

static inline void ioport__write8(u8 *data, u8 value)
{
	*data		 = value;
}

static inline void ioport__write16(u16 *data, u16 value)
{
	*data		 = cpu_to_le16(value);
}

static inline void ioport__write32(u32 *data, u32 value)
{
	*data		 = cpu_to_le32(value);
}

#endif /* KVM__IOPORT_H */
