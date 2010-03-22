#include "kvm/cpu.h"

#include <linux/kvm.h>
#include <stdbool.h>
#include <stdlib.h>
#include <fcntl.h>

struct kvm {
	int			fd;		/* /dev/kvm */
	int			vmfd;
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

	ret = ioctl(self->fd, KVM_CHECK_EXTENSION, extension);
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
	struct kvm *self;
	int ret;

	self = kvm__new();

	self->fd = open("/dev/kvm", O_RDWR);
	if (self->fd < 0)
		die("open");

	ret = ioctl(self->fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION)
		die("ioctl");

	self->vmfd = ioctl(self->fd, KVM_CREATE_VM, 0);
	if (self->vmfd < 0)
		die("open");

	if (!kvm__supports_extension(self, KVM_CAP_USER_MEMORY))
		die("KVM_CAP_USER_MEMORY");

	return self;
}

int main(int argc, char *argv[])
{
	struct cpu *cpu;
	struct kvm *kvm;
	int ret;

	kvm = kvm__init();

	cpu = cpu__new();

	cpu__reset(cpu);

	return 0;
}
