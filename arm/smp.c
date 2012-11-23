#include "kvm/kvm.h"

extern u8 smp_pen_start, smp_pen_end, smp_jump_addr;

static int smp_pen_init(struct kvm *kvm)
{
	unsigned long pen_size, pen_start, jump_offset;

	if (!(kvm->nrcpus > 1))
		return 0;

	pen_size = &smp_pen_end - &smp_pen_start;
	pen_start = kvm->arch.smp_pen_guest_start;
	jump_offset = &smp_jump_addr - &smp_pen_start;

	kvm->arch.smp_jump_guest_start = pen_start + jump_offset;
	memcpy(guest_flat_to_host(kvm, pen_start), &smp_pen_start, pen_size);

	return 0;
}
firmware_init(smp_pen_init);
