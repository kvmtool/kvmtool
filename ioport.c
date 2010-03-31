#include "kvm/kvm.h"

#include <stdio.h>

static void kvm__emulate_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	fprintf(stderr, "%s port=%x, size=%d, count=%" PRIu32 "\n", __func__, port, size, count);
}

static void kvm__emulate_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	fprintf(stderr, "%s port=%x, size=%d, count=%" PRIu32 "\n", __func__, port, size, count);
}

void kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count)
{
	if (direction == KVM_EXIT_IO_IN)
		kvm__emulate_io_in(self, port, data, size, count);
	else
		kvm__emulate_io_out(self, port, data, size, count);
}
