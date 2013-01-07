#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

#include "kvm/kvm.h"

#include "arm-common/kvm-cpu-arch.h"

#define ARM_VCPU_FEATURE_FLAGS(kvm, cpuid)	{			\
	[0] = (!!(cpuid) << KVM_ARM_VCPU_POWER_OFF),			\
}

#endif /* KVM__KVM_CPU_ARCH_H */
