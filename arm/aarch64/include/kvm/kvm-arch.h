#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#define ARM_KERN_OFFSET(kvm)	((kvm)->cfg.arch.aarch32_guest	?	\
				0x8000				:	\
				0x80000)

#define ARM_MAX_MEMORY(kvm)	((kvm)->cfg.arch.aarch32_guest	?	\
				ARM_LOMAP_MAX_MEMORY		:	\
				ARM_HIMAP_MAX_MEMORY)

#include "arm-common/kvm-arch.h"

#endif /* KVM__KVM_ARCH_H */
