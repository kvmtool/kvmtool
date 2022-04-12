#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

#include "kvm/kvm.h"

#include "arm-common/kvm-cpu-arch.h"

#define ARM_MPIDR_HWID_BITMASK	0xFFFFFF
#define ARM_CPU_ID		0, 0, 0
#define ARM_CPU_ID_MPIDR	5

static inline void kvm_cpu__select_features(struct kvm *kvm,
					    struct kvm_vcpu_init *init) { }
static inline int kvm_cpu__configure_features(struct kvm_cpu *vcpu)
{
	return 0;
}

static inline int kvm_cpu__teardown_pvtime(struct kvm *kvm)
{
	return 0;
}

#endif /* KVM__KVM_CPU_ARCH_H */
