#include "kvm/kvm.h"
#include "kvm/util.h"

#include <sys/ioctl.h>
#include <stdlib.h>
#include <assert.h>

#define	MAX_KVM_CPUID_ENTRIES		100

void kvm__setup_cpuid(struct kvm *self)
{
	struct kvm_cpuid2 *kvm_cpuid;

	kvm_cpuid = calloc(1, sizeof(*kvm_cpuid) + MAX_KVM_CPUID_ENTRIES * sizeof(*kvm_cpuid->entries));

	kvm_cpuid->nent = MAX_KVM_CPUID_ENTRIES;
	if (ioctl(self->sys_fd, KVM_GET_SUPPORTED_CPUID, kvm_cpuid) < 0)
		die_perror("KVM_GET_SUPPORTED_CPUID failed");

	if (ioctl(self->vcpu_fd, KVM_SET_CPUID2, kvm_cpuid) < 0)
		die_perror("KVM_SET_CPUID2 failed");

	free(kvm_cpuid);
}
