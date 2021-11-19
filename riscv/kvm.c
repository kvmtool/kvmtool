#include "kvm/kvm.h"
#include "kvm/util.h"
#include "kvm/fdt.h"

#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/sizes.h>

struct kvm_ext kvm_req_ext[] = {
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
	/* TODO: */
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	/* TODO: */
}

void kvm__arch_read_term(struct kvm *kvm)
{
	/* TODO: */
}

void kvm__arch_set_cmdline(char *cmdline, bool video)
{
	/* TODO: */
}

void kvm__arch_init(struct kvm *kvm, const char *hugetlbfs_path, u64 ram_size)
{
	/* TODO: */
}

bool kvm__arch_load_kernel_image(struct kvm *kvm, int fd_kernel, int fd_initrd,
				 const char *kernel_cmdline)
{
	/* TODO: */
	return true;
}

bool kvm__load_firmware(struct kvm *kvm, const char *firmware_filename)
{
	/* TODO: Firmware loading to be supported later. */
	return false;
}

int kvm__arch_setup_firmware(struct kvm *kvm)
{
	return 0;
}
