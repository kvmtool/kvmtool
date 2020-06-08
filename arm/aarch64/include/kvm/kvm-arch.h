#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

struct kvm;
unsigned long long kvm__arch_get_kern_offset(struct kvm *kvm, int fd);

#define ARM_MAX_MEMORY(kvm)	((kvm)->cfg.arch.aarch32_guest	?	\
				ARM_LOMAP_MAX_MEMORY		:	\
				ARM_HIMAP_MAX_MEMORY)

#include "arm-common/kvm-arch.h"

#endif /* KVM__KVM_ARCH_H */
