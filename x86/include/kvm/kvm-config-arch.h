#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	int vidmode;
};

#define OPT_ARCH_RUN(pfx, cfg)						\
	pfx,								\
	OPT_GROUP("BIOS options:"),					\
	OPT_INTEGER('\0', "vidmode", &(cfg)->vidmode, "Video mode"),

#endif /* KVM__KVM_CONFIG_ARCH_H */
