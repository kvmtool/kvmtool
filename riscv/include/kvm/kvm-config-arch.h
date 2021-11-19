#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	const char	*dump_dtb_filename;
};

#define OPT_ARCH_RUN(pfx, cfg)						\
	pfx,								\
	OPT_STRING('\0', "dump-dtb", &(cfg)->dump_dtb_filename,		\
		   ".dtb file", "Dump generated .dtb to specified file"),

#endif /* KVM__KVM_CONFIG_ARCH_H */
