#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

#include "kvm/kvm.h"

#include "arm-common/kvm-cpu-arch.h"

#define ARM_VCPU_FEATURE_FLAGS(kvm, cpuid)	{				\
	[0] = ((!!(cpuid) << KVM_ARM_VCPU_POWER_OFF) |				\
	       (!!(kvm)->cfg.arch.aarch32_guest << KVM_ARM_VCPU_EL1_32BIT))	\
}

#define ARM_MPIDR_HWID_BITMASK	0xFF00FFFFFFUL
#define ARM_CPU_ID		3, 0, 0, 0
#define ARM_CPU_ID_MPIDR	5

#endif /* KVM__KVM_CPU_ARCH_H */
