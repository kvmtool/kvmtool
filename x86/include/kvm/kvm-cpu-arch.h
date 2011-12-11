#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

/* Architecture-specific kvm_cpu definitions. */

#include <linux/kvm.h>	/* for struct kvm_regs */

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

#endif /* KVM__KVM_CPU_ARCH_H */
