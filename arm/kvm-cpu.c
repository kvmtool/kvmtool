#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"

static int debug_fd;

void kvm_cpu__set_debug_fd(int fd)
{
	debug_fd = fd;
}

int kvm_cpu__get_debug_fd(void)
{
	return debug_fd;
}

static struct kvm_arm_target *kvm_arm_targets[KVM_ARM_NUM_TARGETS];
int kvm_cpu__register_kvm_arm_target(struct kvm_arm_target *target)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(kvm_arm_targets); ++i) {
		if (!kvm_arm_targets[i]) {
			kvm_arm_targets[i] = target;
			return 0;
		}
	}

	return -ENOSPC;
}

struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id)
{
	struct kvm_cpu *vcpu;
	int coalesced_offset, mmap_size, err = -1;
	unsigned int i;
	struct kvm_vcpu_init vcpu_init = {
		.features = ARM_VCPU_FEATURE_FLAGS(kvm, cpu_id)
	};

	vcpu = calloc(1, sizeof(struct kvm_cpu));
	if (!vcpu)
		return NULL;

	vcpu->vcpu_fd = ioctl(kvm->vm_fd, KVM_CREATE_VCPU, cpu_id);
	if (vcpu->vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU ioctl");

	mmap_size = ioctl(kvm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

	vcpu->kvm_run = mmap(NULL, mmap_size, PROT_RW, MAP_SHARED,
			     vcpu->vcpu_fd, 0);
	if (vcpu->kvm_run == MAP_FAILED)
		die("unable to mmap vcpu fd");

	/* Find an appropriate target CPU type. */
	for (i = 0; i < ARRAY_SIZE(kvm_arm_targets); ++i) {
		vcpu_init.target = kvm_arm_targets[i]->id;
		err = ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_INIT, &vcpu_init);
		if (!err)
			break;
	}

	if (err || kvm_arm_targets[i]->init(vcpu))
		die("Unable to initialise ARM vcpu");

	coalesced_offset = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION,
				 KVM_CAP_COALESCED_MMIO);
	if (coalesced_offset)
		vcpu->ring = (void *)vcpu->kvm_run +
			     (coalesced_offset * PAGE_SIZE);

	/* Populate the vcpu structure. */
	vcpu->kvm		= kvm;
	vcpu->cpu_id		= cpu_id;
	vcpu->cpu_type		= vcpu_init.target;
	vcpu->is_running	= true;
	return vcpu;
}

void kvm_cpu__arch_nmi(struct kvm_cpu *cpu)
{
}

void kvm_cpu__delete(struct kvm_cpu *vcpu)
{
	free(vcpu);
}

bool kvm_cpu__handle_exit(struct kvm_cpu *vcpu)
{
	return false;
}

bool kvm_cpu__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len,
			   u8 is_write)
{
	if (arm_addr_in_virtio_mmio_region(phys_addr))
		return kvm__emulate_mmio(kvm, phys_addr, data, len, is_write);
	else if (arm_addr_in_pci_mmio_region(phys_addr))
		die("PCI emulation not supported on ARM!");

	return false;
}

void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu)
{
}
