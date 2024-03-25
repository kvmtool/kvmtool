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
	OPT_BOOLEAN('\0', "disable-smstateen",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SMSTATEEN],	\
		    "Disable Smstateen Extension"),			\
	OPT_BOOLEAN('\0', "disable-ssaia",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SSAIA],	\
		    "Disable Ssaia Extension"),				\
	OPT_BOOLEAN('\0', "disable-sstc",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SSTC],	\
		    "Disable Sstc Extension"),				\
	OPT_BOOLEAN('\0', "disable-svinval",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SVINVAL],	\
		    "Disable Svinval Extension"),			\
	OPT_BOOLEAN('\0', "disable-svnapot",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SVNAPOT],	\
		    "Disable Svnapot Extension"),			\
	OPT_BOOLEAN('\0', "disable-svpbmt",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_SVPBMT],	\
		    "Disable Svpbmt Extension"),			\
	OPT_BOOLEAN('\0', "disable-zba",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBA],	\
		    "Disable Zba Extension"),				\
	OPT_BOOLEAN('\0', "disable-zbb",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBB],	\
		    "Disable Zbb Extension"),				\
	OPT_BOOLEAN('\0', "disable-zbc",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBC],	\
		    "Disable Zbc Extension"),				\
	OPT_BOOLEAN('\0', "disable-zbkb",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBKB],	\
		    "Disable Zbkb Extension"),				\
	OPT_BOOLEAN('\0', "disable-zbkc",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBKC],	\
		    "Disable Zbkc Extension"),				\
	OPT_BOOLEAN('\0', "disable-zbkx",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBKX],	\
		    "Disable Zbkx Extension"),				\
	OPT_BOOLEAN('\0', "disable-zbs",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZBS],	\
		    "Disable Zbs Extension"),				\
	OPT_BOOLEAN('\0', "disable-zfh",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZFH],	\
		    "Disable Zfh Extension"),				\
	OPT_BOOLEAN('\0', "disable-zfhmin",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZFHMIN],	\
		    "Disable Zfhmin Extension"),			\
	OPT_BOOLEAN('\0', "disable-zicbom",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZICBOM],	\
		    "Disable Zicbom Extension"),			\
	OPT_BOOLEAN('\0', "disable-zicboz",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZICBOZ],	\
		    "Disable Zicboz Extension"),			\
	OPT_BOOLEAN('\0', "disable-zicntr",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZICNTR],	\
		    "Disable Zicntr Extension"),			\
	OPT_BOOLEAN('\0', "disable-zicond",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZICOND],	\
		    "Disable Zicond Extension"),			\
	OPT_BOOLEAN('\0', "disable-zicsr",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZICSR],	\
		    "Disable Zicsr Extension"),				\
	OPT_BOOLEAN('\0', "disable-zifencei",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZIFENCEI],	\
		    "Disable Zifencei Extension"),			\
	OPT_BOOLEAN('\0', "disable-zihintntl",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZIHINTNTL],	\
		    "Disable Zihintntl Extension"),			\
	OPT_BOOLEAN('\0', "disable-zihintpause",			\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZIHINTPAUSE],\
		    "Disable Zihintpause Extension"),			\
	OPT_BOOLEAN('\0', "disable-zihpm",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZIHPM],	\
		    "Disable Zihpm Extension"),				\
	OPT_BOOLEAN('\0', "disable-zknd",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKND],	\
		    "Disable Zknd Extension"),				\
	OPT_BOOLEAN('\0', "disable-zkne",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKNE],	\
		    "Disable Zkne Extension"),				\
	OPT_BOOLEAN('\0', "disable-zknh",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKNH],	\
		    "Disable Zknh Extension"),				\
	OPT_BOOLEAN('\0', "disable-zkr",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKR],	\
		    "Disable Zkr Extension"),				\
	OPT_BOOLEAN('\0', "disable-zksed",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKSED],	\
		    "Disable Zksed Extension"),				\
	OPT_BOOLEAN('\0', "disable-zksh",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKSH],	\
		    "Disable Zksh Extension"),				\
	OPT_BOOLEAN('\0', "disable-zkt",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZKT],	\
		    "Disable Zkt Extension"),				\
	OPT_BOOLEAN('\0', "disable-zvbb",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVBB],	\
		    "Disable Zvbb Extension"),				\
	OPT_BOOLEAN('\0', "disable-zvbc",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVBC],	\
		    "Disable Zvbc Extension"),				\
	OPT_BOOLEAN('\0', "disable-zvkb",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKB],	\
		    "Disable Zvkb Extension"),				\
	OPT_BOOLEAN('\0', "disable-zvkg",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKG],	\
		    "Disable Zvkg Extension"),				\
	OPT_BOOLEAN('\0', "disable-zvkned",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKNED],	\
		    "Disable Zvkned Extension"),			\
	OPT_BOOLEAN('\0', "disable-zvknha",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKNHA],	\
		    "Disable Zvknha Extension"),			\
	OPT_BOOLEAN('\0', "disable-zvknhb",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKNHB],	\
		    "Disable Zvknhb Extension"),			\
	OPT_BOOLEAN('\0', "disable-zvksed",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKSED],	\
		    "Disable Zvksed Extension"),			\
	OPT_BOOLEAN('\0', "disable-zvksh",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKSH],	\
		    "Disable Zvksh Extension"),				\
	OPT_BOOLEAN('\0', "disable-zvkt",				\
		    &(cfg)->ext_disabled[KVM_RISCV_ISA_EXT_ZVKT],	\
		    "Disable Zvkt Extension"),				\
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
		    "Disable SBI Vendor Extensions"),			\
	OPT_BOOLEAN('\0', "disable-sbi-dbcn",				\
		    &(cfg)->sbi_ext_disabled[KVM_RISCV_SBI_EXT_DBCN],	\
		    "Disable SBI DBCN Extension"),

#endif /* KVM__KVM_CONFIG_ARCH_H */
