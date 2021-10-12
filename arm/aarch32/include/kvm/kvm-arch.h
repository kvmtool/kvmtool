#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include <linux/sizes.h>

#define kvm__arch_get_kern_offset(...)	0x8000

#define ARM_MAX_MEMORY(...)	ARM_LOMAP_MAX_MEMORY

#define MAX_PAGE_SIZE	SZ_4K

#include "arm-common/kvm-arch.h"

#endif /* KVM__KVM_ARCH_H */
