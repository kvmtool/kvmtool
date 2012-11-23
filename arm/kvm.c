#include "kvm/kvm.h"
#include "kvm/term.h"
#include "kvm/util.h"
#include "kvm/virtio-console.h"

#include "arm-common/gic.h"

#include <linux/kernel.h>
#include <linux/kvm.h>

struct kvm_ext kvm_req_ext[] = {
	{ DEFINE_KVM_EXT(KVM_CAP_IRQCHIP) },
	{ DEFINE_KVM_EXT(KVM_CAP_ONE_REG) },
	{ 0, 0 },
};

bool kvm__arch_cpu_supports_vm(void)
{
	/* The KVM capability check is enough. */
	return true;
}

void kvm__init_ram(struct kvm *kvm)
{
	int err;
	u64 phys_start, phys_size;
	void *host_mem;

	phys_start	= ARM_MEMORY_AREA;
	phys_size	= kvm->ram_size;
	host_mem	= kvm->ram_start;

	err = kvm__register_mem(kvm, phys_start, phys_size, host_mem);
	if (err)
		die("Failed to register %lld bytes of memory at physical "
		    "address 0x%llx [err %d]", phys_size, phys_start, err);

	kvm->arch.memory_guest_start = phys_start;
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	munmap(kvm->ram_start, kvm->ram_size);
}

void kvm__arch_periodic_poll(struct kvm *kvm)
{
	if (term_readable(0))
		virtio_console__inject_interrupt(kvm);
}

void kvm__arch_set_cmdline(char *cmdline, bool video)
{
}

void kvm__arch_init(struct kvm *kvm, const char *hugetlbfs_path, u64 ram_size)
{
	/* Allocate guest memory. */
	kvm->ram_size = min(ram_size, (u64)ARM_MAX_MEMORY);
	kvm->ram_start = mmap_anon_or_hugetlbfs(kvm, hugetlbfs_path, kvm->ram_size);
	if (kvm->ram_start == MAP_FAILED)
		die("Failed to map %lld bytes for guest memory (%d)",
		    kvm->ram_size, errno);
	madvise(kvm->ram_start, kvm->ram_size, MADV_MERGEABLE);

	/* Initialise the virtual GIC. */
	if (gic__init_irqchip(kvm))
		die("Failed to initialise virtual GIC");
}
