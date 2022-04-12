#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

int vcpu_affinity_parser(const struct option *opt, const char *arg, int unset);

#define ARM_OPT_ARCH_RUN(cfg)						\
	OPT_BOOLEAN('\0', "aarch32", &(cfg)->aarch32_guest,		\
			"Run AArch32 guest"),				\
	OPT_BOOLEAN('\0', "pmu", &(cfg)->has_pmuv3,			\
			"Create PMUv3 device. The emulated PMU will be" \
			" set to the PMU associated with the"		\
			" main thread, unless --vcpu-affinity is set"),	\
	OPT_BOOLEAN('\0', "disable-mte", &(cfg)->mte_disabled,		\
			"Disable Memory Tagging Extension"),		\
	OPT_CALLBACK('\0', "vcpu-affinity", kvm, "cpulist",  		\
			"Specify the CPU affinity that will apply to "	\
			"all VCPUs", vcpu_affinity_parser, kvm),	\
	OPT_U64('\0', "kaslr-seed", &(cfg)->kaslr_seed,			\
			"Specify random seed for Kernel Address Space "	\
			"Layout Randomization (KASLR)"),		\
	OPT_BOOLEAN('\0', "no-pvtime", &(cfg)->no_pvtime, "Disable"	\
			" stolen time"),
#include "arm-common/kvm-config-arch.h"

#endif /* KVM__KVM_CONFIG_ARCH_H */
