#include <linux/kvm.h>

#include <asm/bootparam.h>

#include <inttypes.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

/*
 * Compatibility code. Remove this when we move to tools/kvm.
 */
#ifndef KVM_EXIT_INTERNAL_ERROR
# define KVM_EXIT_INTERNAL_ERROR		17
#endif

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	int			vcpu_fd;	/* For VCPU ioctls() */
	struct kvm_run		*kvm_run;

	uint64_t		ram_size;
	void			*ram_start;
};

static void die_perror(const char *s)
{
	perror(s);
	exit(1);
}

static void die(const char *format, ...)
{
        va_list ap;

        va_start(ap, format);
        vprintf(format, ap);
        va_end(ap);

        printf("\n");
	exit(1);
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
	long page_size;
	int mmap_size;
	int ret;

	self = kvm__new();

	self->sys_fd = open("/dev/kvm", O_RDWR);
	if (self->sys_fd < 0)
		die_perror("open");

	ret = ioctl(self->sys_fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION)
		die_perror("KVM_API_VERSION ioctl");

	self->vm_fd = ioctl(self->sys_fd, KVM_CREATE_VM, 0);
	if (self->vm_fd < 0)
		die_perror("KVM_CREATE_VM ioctl");

	if (!kvm__supports_extension(self, KVM_CAP_USER_MEMORY))
		die("KVM_CAP_USER_MEMORY is not supported");

	self->ram_size		= 64UL * 1024UL * 1024UL;

	page_size	= sysconf(_SC_PAGESIZE);
	if (posix_memalign(&self->ram_start, page_size, self->ram_size) != 0)
		die("out of memory");

	mem = (struct kvm_userspace_memory_region) {
		.slot			= 0,
		.guest_phys_addr	= 0x0UL,
		.memory_size		= self->ram_size,
		.userspace_addr		= (unsigned long) self->ram_start,
	};

	ret = ioctl(self->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem, 1);
	if (ret < 0)
		die_perror("KVM_SET_USER_MEMORY_REGION ioctl");

	if (!kvm__supports_extension(self, KVM_CAP_SET_TSS_ADDR))
		die("KVM_CAP_SET_TSS_ADDR is not supported");

	ret = ioctl(self->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000);
	if (ret < 0)
		die_perror("KVM_SET_TSS_ADDR ioctl");

	self->vcpu_fd = ioctl(self->vm_fd, KVM_CREATE_VCPU, 0);
	if (self->vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU ioctl");

	mmap_size = ioctl(self->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

	self->kvm_run = mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, self->vcpu_fd, 0);
	if (self->kvm_run == MAP_FAILED)
		die("unable to mmap vcpu fd");

	return self;
}

static void kvm__run(struct kvm *self)
{
	int ret;

	ret = ioctl(self->vcpu_fd, KVM_RUN, 0);
	if (ret < 0)
		die_perror("KVM_RUN ioctl");
}

static const char *BZIMAGE_MAGIC	= "HdrS";

static int load_bzimage(struct kvm *kvm, int fd)
{
	struct boot_params boot;

	read(fd, &boot, sizeof(boot));

        if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)) != 0)
		return -1;

	return 0;
}

static void kvm__load_kernel(struct kvm *kvm, const char *kernel_filename)
{
	int fd;

	fd = open(kernel_filename, O_RDONLY);
	if (fd < 0)
		die("unable to open kernel");

	if (load_bzimage(kvm, fd) < 0)
		die("%s is not a valid bzImage", kernel_filename);
}

static const char *exit_reasons[] = {
	[KVM_EXIT_UNKNOWN]		= "unknown",
	[KVM_EXIT_EXCEPTION]		= "exception",
	[KVM_EXIT_IO]			= "io",
	[KVM_EXIT_HYPERCALL]		= "hypercall",
	[KVM_EXIT_DEBUG]		= "debug",
	[KVM_EXIT_HLT]			= "hlt",
	[KVM_EXIT_MMIO]			= "mmio",
	[KVM_EXIT_IRQ_WINDOW_OPEN]	= "irq window open",
	[KVM_EXIT_SHUTDOWN]		= "shutdown",
	[KVM_EXIT_FAIL_ENTRY]		= "fail entry",
	[KVM_EXIT_INTR]			= "intr",
	[KVM_EXIT_SET_TPR]		= "set tpr",
	[KVM_EXIT_TPR_ACCESS]		= "trp access",
	[KVM_EXIT_S390_SIEIC]		= "s390 sieic",
	[KVM_EXIT_S390_RESET]		= "s390 reset",
	[KVM_EXIT_DCR]			= "dcr",
	[KVM_EXIT_NMI]			= "dmi",
	[KVM_EXIT_INTERNAL_ERROR]	= "internal error",
};

static void usage(char *argv[])
{
	fprintf(stderr, "  usage: %s <kernel-image>\n", argv[0]);
	exit(1);
}

int main(int argc, char *argv[])
{
	const char *kernel_filename;
	struct kvm *kvm;
	int ret;

	if (argc < 2)
		usage(argv);

	kernel_filename = argv[1];

	kvm = kvm__init();

	kvm__load_kernel(kvm, kernel_filename);

	kvm__run(kvm);

	fprintf(stderr, "KVM exit reason: %" PRIu32 " (\"%s\")\n",
		kvm->kvm_run->exit_reason, exit_reasons[kvm->kvm_run->exit_reason]);

	return 0;
}
