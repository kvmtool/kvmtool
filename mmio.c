#include "kvm/kvm.h"

#include <stdio.h>
#include <linux/types.h>

static const char *to_direction(u8 is_write)
{
	if (is_write)
		return "write";

	return "read";
}

bool kvm__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write)
{
	fprintf(stderr, "Warning: Ignoring MMIO %s at %016llx (length %u)\n",
		to_direction(is_write), phys_addr, len);

	return true;
}
