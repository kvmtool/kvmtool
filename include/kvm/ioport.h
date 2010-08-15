#ifndef KVM__IOPORT_H
#define KVM__IOPORT_H

#include <stdbool.h>
#include <stdint.h>

/* some ports we reserve for own use */
#define IOPORT_DBG	0xe0
#define IOPORT_VIRTIO	0xc200

struct kvm;

struct ioport_operations {
	bool (*io_in)(struct kvm *self, uint16_t port, void *data, int size, uint32_t count);
	bool (*io_out)(struct kvm *self, uint16_t port, void *data, int size, uint32_t count);
};

void ioport__register(uint16_t port, struct ioport_operations *ops, int count);

#endif /* KVM__IOPORT_H */
