#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	const char	*dump_dtb_filename;
	u64		custom_mvendorid;
	u64		custom_marchid;
	u64		custom_mimpid;
	bool		ext_disabled[KVM_RISCV_ISA_EXT_MAX];
	bool		sbi_ext_disabled[KVM_RISCV_SBI_EXT_MAX];
};

#define OPT_ARCH_RUN(pfx, cfg)						\
	pfx,								\
	OPT_STRING('\0', "dump-dtb", &(cfg)->dump_dtb_filename,		\
		   ".dtb file", "Dump generated .dtb to specified file"),\
	OPT_U64('\0', "custom-mvendorid",				\
		&(cfg)->custom_mvendorid,				\
		"Show custom mvendorid to Guest VCPU"),			\
	OPT_U64('\0', "custom-marchid",					\
		&(cfg)->custom_marchid,					\
		"Show custom marchid to Guest VCPU"),			\
	OPT_U64('\0', "custom-mimpid",					\
		&(cfg)->custom_mimpid,					\
		"Show custom mimpid to Guest VCPU"),			\
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
		    "Disable Zihintpause Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-legacy",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_V01],	\
		    "Disable SBI Legacy Extensions"),			\
	OPT_BOOLEAN('\0', "disable-sbi-time",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_TIME],	\
		    "Disable SBI Time Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-ipi",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_IPI],	\
		    "Disable SBI IPI Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-rfence",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_RFENCE],	\
		    "Disable SBI RFence Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-srst",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_SRST],	\
		    "Disable SBI SRST Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-hsm",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_HSM],	\
		    "Disable SBI HSM Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-pmu",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_PMU],	\
		    "Disable SBI PMU Extension"),			\
	OPT_BOOLEAN('\0', "disable-sbi-experimental",			\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_EXPERIMENTAL],\
		    "Disable SBI Experimental Extensions"),		\
	OPT_BOOLEAN('\0', "disable-sbi-vendor",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_VENDOR],	\
		    "Disable SBI Vendor Extensions"),

#endif /* KVM__KVM_CONFIG_ARCH_H */
