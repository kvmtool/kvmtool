#ifndef ARM_COMMON__KVM_CPU_ARCH_H
#define ARM_COMMON__KVM_CPU_ARCH_H

#include <linux/kvm.h>
#include <pthread.h>
#include <stdbool.h>

struct kvm;

struct kvm_cpu {
	pthread_t	thread;

	unsigned long	cpu_id;
	unsigned long	cpu_type;

	struct kvm	*kvm;
	int		vcpu_fd;
	struct kvm_run	*kvm_run;

	u8		is_running;
	u8		paused;
	u8		needs_nmi;

	struct kvm_coalesced_mmio_ring	*ring;

	void		(*generate_fdt_nodes)(void *fdt, struct kvm* kvm,
					      u32 gic_phandle);
};

struct kvm_arm_target {
	u32	id;
	int	(*init)(struct kvm_cpu *vcpu);
};

int kvm_cpu__register_kvm_arm_target(struct kvm_arm_target *target);

static inline bool kvm_cpu__emulate_io(struct kvm *kvm, u16 port, void *data,
				       int direction, int size, u32 count)
{
	return false;
}

bool kvm_cpu__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len,
			   u8 is_write);

#endif /* ARM_COMMON__KVM_CPU_ARCH_H */
