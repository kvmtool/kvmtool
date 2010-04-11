#ifndef KVM__IOPORT_H
#define KVM__IOPORT_H

#include <stdbool.h>
#include <stdint.h>

struct kvm;

struct ioport_operations {
	bool (*io_in)(struct kvm *self, uint16_t port, void *data, int size, uint32_t count);
	bool (*io_out)(struct kvm *self, uint16_t port, void *data, int size, uint32_t count);
};

void ioport__register(uint16_t port, struct ioport_operations *ops);

#endif /* KVM__IOPORT_H */
