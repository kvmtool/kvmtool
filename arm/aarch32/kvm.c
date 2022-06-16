#include "kvm/kvm.h"

void kvm__arch_validate_cfg(struct kvm *kvm)
{
	if (kvm->cfg.ram_size > ARM_LOMAP_MAX_MEMORY) {
		die("RAM size 0x%llx exceeds maximum allowed 0x%llx",
		    kvm->cfg.ram_size, ARM_LOMAP_MAX_MEMORY);
	}
}
