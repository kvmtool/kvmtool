#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#define ARM_GIC_DIST_SIZE	0x1000
#define ARM_GIC_CPUI_SIZE	0x2000

#define ARM_KERN_OFFSET(...)	0x8000

#define ARM_MAX_MEMORY(...)	ARM_LOMAP_MAX_MEMORY

#include "arm-common/kvm-arch.h"

#endif /* KVM__KVM_ARCH_H */
