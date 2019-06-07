#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

#include "kvm/kvm.h"

#include "arm-common/kvm-cpu-arch.h"

#define ARM_VCPU_FEATURE_FLAGS(kvm, cpuid)	{				\
	[0] = ((!!(cpuid) << KVM_ARM_VCPU_POWER_OFF) |				\
	       (!!(kvm)->cfg.arch.aarch32_guest << KVM_ARM_VCPU_EL1_32BIT) |	\
	       (!!(kvm)->cfg.arch.has_pmuv3 << KVM_ARM_VCPU_PMU_V3))		\
}

#define ARM_MPIDR_HWID_BITMASK	0xFF00FFFFFFUL
#define ARM_CPU_ID		3, 0, 0, 0
#define ARM_CPU_ID_MPIDR	5
#define ARM_CPU_CTRL		3, 0, 1, 0
#define ARM_CPU_CTRL_SCTLR_EL1	0

void kvm_cpu__select_features(struct kvm *kvm, struct kvm_vcpu_init *init);
int kvm_cpu__configure_features(struct kvm_cpu *vcpu);

#endif /* KVM__KVM_CPU_ARCH_H */
