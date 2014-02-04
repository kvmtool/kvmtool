#ifndef ARM_COMMON__KVM_CONFIG_ARCH_H
#define ARM_COMMON__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	const char	*dump_dtb_filename;
	unsigned int	force_cntfrq;
	bool		virtio_trans_pci;
	bool		aarch32_guest;
};

#define OPT_ARCH_RUN(pfx, cfg)							\
	pfx,									\
	ARM_OPT_ARCH_RUN(cfg)							\
	OPT_STRING('\0', "dump-dtb", &(cfg)->dump_dtb_filename,			\
		   ".dtb file", "Dump generated .dtb to specified file"),	\
	OPT_UINTEGER('\0', "override-bad-firmware-cntfrq", &(cfg)->force_cntfrq,\
		     "Specify Generic Timer frequency in guest DT to "		\
		     "work around buggy secure firmware *Firmware should be "	\
		     "updated to program CNTFRQ correctly*"),			\
	OPT_BOOLEAN('\0', "force-pci", &(cfg)->virtio_trans_pci,		\
		    "Force virtio devices to use PCI as their default "		\
		    "transport"),

#endif /* ARM_COMMON__KVM_CONFIG_ARCH_H */
