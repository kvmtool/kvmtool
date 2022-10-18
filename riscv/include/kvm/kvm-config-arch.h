#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	const char	*dump_dtb_filename;
	bool		ext_disabled[KVM_RISCV_ISA_EXT_MAX];
};

#define OPT_ARCH_RUN(pfx, cfg)						\
	pfx,								\
	OPT_STRING('\0', "dump-dtb", &(cfg)->dump_dtb_filename,		\
		   ".dtb file", "Dump generated .dtb to specified file"),\
	OPT_BOOLEAN('\0', "disable-sstc",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SSTC],	\
		    "Disable Sstc Extension"),				\
	OPT_BOOLEAN('\0', "disable-svinval",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SVINVAL],	\
		    "Disable Svinval Extension"),			\
	OPT_BOOLEAN('\0', "disable-svpbmt",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SVPBMT],	\
		    "Disable Svpbmt Extension"),			\
	OPT_BOOLEAN('\0', "disable-zicbom",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZICBOM],	\
		    "Disable Zicbom Extension"),			\
	OPT_BOOLEAN('\0', "disable-zihintpause",			\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZIHINTPAUSE],\
		    "Disable Zihintpause Extension"),

#endif /* KVM__KVM_CONFIG_ARCH_H */
