#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include <linux/byteorder.h>
#include <linux/types.h>

#define ARM_PVTIME_STRUCT_SIZE		(64)

static void *usr_mem;

static int pvtime__alloc_region(struct kvm *kvm)
{
	char *mem;
	int ret = 0;

	mem = mmap(NULL, ARM_PVTIME_SIZE, PROT_RW,
		   MAP_ANON_NORESERVE, -1, 0);
	if (mem == MAP_FAILED)
		return -errno;

	ret = kvm__register_ram(kvm, ARM_PVTIME_BASE,
				ARM_PVTIME_SIZE, mem);
	if (ret) {
		munmap(mem, ARM_PVTIME_SIZE);
		return ret;
	}

	usr_mem = mem;
	return ret;
}

static int pvtime__teardown_region(struct kvm *kvm)
{
	if (usr_mem == NULL)
		return 0;

	kvm__destroy_mem(kvm, ARM_PVTIME_BASE,
			 ARM_PVTIME_SIZE, usr_mem);
	munmap(usr_mem, ARM_PVTIME_SIZE);
	usr_mem = NULL;
	return 0;
}

int kvm_cpu__setup_pvtime(struct kvm_cpu *vcpu)
{
	int ret;
	bool has_stolen_time;
	u64 pvtime_guest_addr = ARM_PVTIME_BASE + vcpu->cpu_id *
		ARM_PVTIME_STRUCT_SIZE;
	struct kvm_config_arch *kvm_cfg = NULL;
	struct kvm_device_attr pvtime_attr = (struct kvm_device_attr) {
		.group	= KVM_ARM_VCPU_PVTIME_CTRL,
		.attr	= KVM_ARM_VCPU_PVTIME_IPA
	};

	kvm_cfg = &vcpu->kvm->cfg.arch;
	if (kvm_cfg->no_pvtime)
		return 0;

	has_stolen_time = kvm__supports_extension(vcpu->kvm,
						  KVM_CAP_STEAL_TIME);
	if (!has_stolen_time) {
		kvm_cfg->no_pvtime = true;
		return 0;
	}

	ret = ioctl(vcpu->vcpu_fd, KVM_HAS_DEVICE_ATTR, &pvtime_attr);
	if (ret) {
		ret = -errno;
		perror("KVM_HAS_DEVICE_ATTR failed\n");
		goto out_err;
	}

	if (!usr_mem) {
		ret = pvtime__alloc_region(vcpu->kvm);
		if (ret) {
			perror("Failed allocating pvtime region\n");
			goto out_err;
		}
	}

	pvtime_attr.addr = (u64)&pvtime_guest_addr;
	ret = ioctl(vcpu->vcpu_fd, KVM_SET_DEVICE_ATTR, &pvtime_attr);
	if (!ret)
		return 0;

	ret = -errno;
	perror("KVM_SET_DEVICE_ATTR failed\n");
	pvtime__teardown_region(vcpu->kvm);
out_err:
	return ret;
}

int kvm_cpu__teardown_pvtime(struct kvm *kvm)
{
	return pvtime__teardown_region(kvm);
}
