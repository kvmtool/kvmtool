#include "kvm/cpu.h"

#include <linux/kvm.h>
#include <stdbool.h>
#include <stdlib.h>
#include <fcntl.h>

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	int			vcpu_fd;	/* For VCPU ioctls() */
};

static void die(const char *s)
{
	perror(s);
	exit(1);
}

static void cpu__reset(struct cpu *self)
{
	self->regs.eip		= 0x000fff0UL;
	self->regs.eflags	= 0x0000002UL;
}

static struct cpu *cpu__new(void)
{
	return calloc(1, sizeof(struct cpu));
}

static inline bool kvm__supports_extension(struct kvm *self, unsigned int extension)
{
	int ret;

	ret = ioctl(self->sys_fd, KVM_CHECK_EXTENSION, extension);
	if (ret < 0)
		return false;

	return ret;
}

static struct kvm *kvm__new(void)
{
	struct kvm *self = calloc(1, sizeof *self);

	if (!self)
		die("out of memory");

	return self;
}

static struct kvm *kvm__init(void)
{
	struct kvm_userspace_memory_region mem;
	struct kvm *self;
	int ret;

	self = kvm__new();

	self->sys_fd = open("/dev/kvm", O_RDWR);
	if (self->sys_fd < 0)
		die("open");

	ret = ioctl(self->sys_fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION)
		die("ioctl");

	self->vm_fd = ioctl(self->sys_fd, KVM_CREATE_VM, 0);
	if (self->vm_fd < 0)
		die("open");

	if (!kvm__supports_extension(self, KVM_CAP_USER_MEMORY))
		die("KVM_CAP_USER_MEMORY");

	mem = (struct kvm_userspace_memory_region) {
		.slot			= 0,
		.guest_phys_addr	= 0x0UL,
		.memory_size		= 64UL * 1024UL * 1024UL,
	};

	ret = ioctl(self->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem, 1);
	if (ret < 0)
		die("ioctl(KVM_SET_USER_MEMORY_REGION)");

	if (!kvm__supports_extension(self, KVM_CAP_SET_TSS_ADDR))
		die("KVM_CAP_SET_TSS_ADDR");

	ret = ioctl(self->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000);
	if (ret < 0)
		die("ioctl(KVM_SET_TSS_ADDR)");

	self->vcpu_fd = ioctl(self->vm_fd, KVM_CREATE_VCPU, 0);
	if (self->vcpu_fd < 0)
		die("ioctl(KVM_CREATE_VCPU)");

	return self;
}

static void kvm__run(struct kvm *self)
{
	int ret;

	ret = ioctl(self->vcpu_fd, KVM_RUN, 0);
	if (ret < 0)
		die("KVM_RUN");
}

int main(int argc, char *argv[])
{
	struct cpu *cpu;
	struct kvm *kvm;
	int ret;

	kvm = kvm__init();

	cpu = cpu__new();

	cpu__reset(cpu);

	kvm__run(kvm);

	return 0;
}
