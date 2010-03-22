#include "kvm/cpu.h"

#include <linux/kvm.h>
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

int main(int argc, char *argv[])
{
	struct cpu *cpu;
	struct kvm kvm;
	int ret;

	kvm.fd = open("/dev/kvm", O_RDWR);
	if (kvm.fd < 0)
		die("open");

	ret = ioctl(kvm.fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION)
		die("ioctl");

	kvm.vmfd = ioctl(kvm.fd, KVM_CREATE_VM, 0);
	if (kvm.vmfd < 0)
		die("open");

	cpu = cpu__new();

	cpu__reset(cpu);

	return 0;
}
