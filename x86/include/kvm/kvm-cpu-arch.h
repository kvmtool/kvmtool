#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

/* Architecture-specific kvm_cpu definitions. */

#include <linux/kvm.h>	/* for struct kvm_regs */
#include "kvm/kvm.h"	/* for kvm__emulate_{mm}io() */
#include <stdbool.h>
#include <pthread.h>

struct kvm;

struct kvm_cpu {
	pthread_t		thread;		/* VCPU thread */

	unsigned long		cpu_id;

	struct kvm		*kvm;		/* parent KVM */
	int			vcpu_fd;	/* For VCPU ioctls() */
	struct kvm_run		*kvm_run;

	struct kvm_regs		regs;
	struct kvm_sregs	sregs;
	struct kvm_fpu		fpu;

	struct kvm_msrs		*msrs;		/* dynamically allocated */

	u8			is_running;
	u8			paused;
	u8			needs_nmi;

	struct kvm_coalesced_mmio_ring	*ring;
};

/*
 * As these are such simple wrappers, let's have them in the header so they'll
 * be cheaper to call:
 */
static inline bool kvm_cpu__emulate_io(struct kvm *kvm, u16 port, void *data, int direction, int size, u32 count)
{
	return kvm__emulate_io(kvm, port, data, direction, size, count);
}

static inline bool kvm_cpu__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write)
{
	return kvm__emulate_mmio(kvm, phys_addr, data, len, is_write);
}

#endif /* KVM__KVM_CPU_ARCH_H */
