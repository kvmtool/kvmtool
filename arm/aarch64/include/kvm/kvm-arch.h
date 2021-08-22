#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

struct kvm;
unsigned long long kvm__arch_get_kern_offset(struct kvm *kvm, int fd);
int kvm__arch_get_ipa_limit(struct kvm *kvm);

#define ARM_MAX_MEMORY(kvm)	({					\
	u64 max_ram;							\
									\
	if ((kvm)->cfg.arch.aarch32_guest) {				\
		max_ram = ARM_LOMAP_MAX_MEMORY;				\
	} else {							\
		int ipabits = kvm__arch_get_ipa_limit(kvm);		\
		if (ipabits <= 0)					\
			max_ram = ARM_HIMAP_MAX_MEMORY;			\
		else							\
			max_ram = (1ULL << ipabits) - ARM_MEMORY_AREA;	\
	}								\
									\
	max_ram;							\
})

#include "arm-common/kvm-arch.h"

#endif /* KVM__KVM_ARCH_H */
