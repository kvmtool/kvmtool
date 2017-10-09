#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

#define ARM_OPT_ARCH_RUN(cfg)						\
	OPT_BOOLEAN('\0', "aarch32", &(cfg)->aarch32_guest,		\
			"Run AArch32 guest"),				\
	OPT_BOOLEAN('\0', "pmu", &(cfg)->has_pmuv3,			\
			"Create PMUv3 device"),				\
	OPT_U64('\0', "kaslr-seed", &(cfg)->kaslr_seed,			\
			"Specify random seed for Kernel Address Space "	\
			"Layout Randomization (KASLR)"),

#include "arm-common/kvm-config-arch.h"

#endif /* KVM__KVM_CONFIG_ARCH_H */
