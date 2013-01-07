#ifndef ARM_COMMON__KVM_CONFIG_ARCH_H
#define ARM_COMMON__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	const char *dump_dtb_filename;
	bool aarch32_guest;
};

#define OPT_ARCH_RUN(pfx, cfg)						\
	pfx,								\
	ARM_OPT_ARCH_RUN(cfg)						\
	OPT_STRING('\0', "dump-dtb", &(cfg)->dump_dtb_filename,		\
		   ".dtb file", "Dump generated .dtb to specified file"),

#endif /* ARM_COMMON__KVM_CONFIG_ARCH_H */
