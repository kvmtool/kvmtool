#include "kvm/kvm.h"

#include <stdio.h>

static const char *to_direction(uint8_t is_write)
{
	if (is_write)
		return "write";

	return "read";
}

bool kvm__emulate_mmio(struct kvm *self, uint64_t phys_addr, uint8_t *data, uint32_t len, uint8_t is_write)
{
	fprintf(stderr, "Warning: Ignoring MMIO %s at %016" PRIx64 " (length %" PRIu32 ")\n",
		to_direction(is_write), phys_addr, len);

	return true;
}
