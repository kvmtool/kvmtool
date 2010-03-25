#include "kvm/kvm.h"

#include <linux/kvm.h>

#include <asm/bootparam.h>

#include <sys/ioctl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#include "util.h"

/*
 * Compatibility code. Remove this when we move to tools/kvm.
 */
#ifndef KVM_EXIT_INTERNAL_ERROR
# define KVM_EXIT_INTERNAL_ERROR		17
#endif

#define DEFINE_KVM_EXIT_REASON(reason) [reason] = #reason

const char *kvm_exit_reasons[] = {
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_UNKNOWN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_EXCEPTION),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_IO),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_HYPERCALL),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_DEBUG),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_HLT),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_MMIO),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_IRQ_WINDOW_OPEN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_SHUTDOWN),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_FAIL_ENTRY),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_INTR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_SET_TPR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_TPR_ACCESS),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_S390_SIEIC),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_S390_RESET),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_DCR),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_NMI),
	DEFINE_KVM_EXIT_REASON(KVM_EXIT_INTERNAL_ERROR),
};

static inline void *guest_addr_to_host(struct kvm *self, unsigned long offset)
{
	return self->ram_start + offset;
}

static bool kvm__supports_extension(struct kvm *self, unsigned int extension)
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

struct kvm *kvm__init(void)
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

void kvm__enable_singlestep(struct kvm *self)
{
	struct kvm_guest_debug debug = {
		.control	= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP,
	};

	if (ioctl(self->vcpu_fd, KVM_SET_GUEST_DEBUG, &debug) < 0)
		warning("KVM_SET_GUEST_DEBUG failed");
}

/* bzImages are loaded at 1 MB by default.  */
#define KERNEL_START_ADDR	(1024ULL * 1024ULL)

static const char *BZIMAGE_MAGIC	= "HdrS";

static uint32_t load_bzimage(struct kvm *kvm, int fd)
{
	struct boot_params boot;
	void *p;
	int nr;

	read(fd, &boot, sizeof(boot));

        if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)) != 0)
		return 0;

	lseek(fd, (boot.hdr.setup_sects+1) * 512, SEEK_SET);

	p = guest_addr_to_host(kvm, KERNEL_START_ADDR);

	while ((nr = read(fd, p, 65536)) > 0)
		p += nr;

	return boot.hdr.code32_start;
}

uint32_t kvm__load_kernel(struct kvm *kvm, const char *kernel_filename)
{
	uint32_t ret;
	int fd;

	fd = open(kernel_filename, O_RDONLY);
	if (fd < 0)
		die("unable to open kernel");

	ret = load_bzimage(kvm, fd);
	if (!ret)
		die("%s is not a valid bzImage", kernel_filename);

	return ret;
}

void kvm__reset_vcpu(struct kvm *self, uint64_t rip)
{
	self->regs = (struct kvm_regs) {
		.rip		= rip,
		.rflags		= 0x0000000000000002ULL,
	};

	if (ioctl(self->vcpu_fd, KVM_SET_REGS, &self->regs) < 0)
		die_perror("KVM_SET_REGS failed");

	self->sregs = (struct kvm_sregs) {
		.cr0		= 0x60000010ULL,
		.cs		= (struct kvm_segment) {
			.selector	= 0xf000UL,
			.base		= 0xffff0000UL,
			.limit		= 0xffffU,
			.type		= 0x0bU,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.ss		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.ds		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.es		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.fs		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.gs		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.tr		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.present	= 1,
			.type		= 0x03U,
		},
		.ldt		= (struct kvm_segment) {
			.limit		= 0xffffU,
			.present	= 1,
			.type		= 0x03U,
		},
		.gdt		= (struct kvm_dtable) {
			.limit		= 0xffffU,
		},
		.idt		= (struct kvm_dtable) {
			.limit		= 0xffffU,
		},
	};

	if (ioctl(self->vcpu_fd, KVM_SET_SREGS, &self->sregs) < 0)
		die_perror("KVM_SET_SREGS failed");
}

void kvm__run(struct kvm *self)
{
	if (ioctl(self->vcpu_fd, KVM_RUN, 0) < 0)
		die_perror("KVM_RUN failed");
}

static void kvm__emulate_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	fprintf(stderr, "%s port=%x, size=%d, count=%" PRIu32 "\n", __func__, port, size, count);
}

static void kvm__emulate_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	fprintf(stderr, "%s port=%x, size=%d, count=%" PRIu32 "\n", __func__, port, size, count);
}

void kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count)
{
	if (direction == KVM_EXIT_IO_IN)
		kvm__emulate_io_in(self, port, data, size, count);
	else
		kvm__emulate_io_out(self, port, data, size, count);
}

static void print_segment(const char *name, struct kvm_segment *seg)
{
	printf(" %s        %04" PRIx16 "      %016" PRIx64 "  %08" PRIx32 "  %02" PRIx8 "    %x %x   %x  %x %x %x\n",
		name, (uint16_t) seg->selector, (uint64_t) seg->base, (uint32_t) seg->limit,
		(uint8_t) seg->type, seg->present, seg->dpl, seg->db, seg->s, seg->l, seg->g);
}

void kvm__show_registers(struct kvm *self)
{
	unsigned long cr0, cr2, cr3;
	unsigned long cr4, cr8;
	unsigned long rax, rbx, rcx;
	unsigned long rdx, rsi, rdi;
	unsigned long rbp,  r8,  r9;
	unsigned long r10, r11, r12;
	unsigned long r13, r14, r15;
	unsigned long rip, rsp;
	struct kvm_sregs sregs;
	unsigned long rflags;
	struct kvm_regs regs;

	if (ioctl(self->vcpu_fd, KVM_GET_REGS, &regs) < 0)
		die("KVM_GET_REGS failed");

	rflags = regs.rflags;

	rip = regs.rip; rsp = regs.rsp;
	rax = regs.rax; rbx = regs.rbx; rcx = regs.rcx;
	rdx = regs.rdx; rsi = regs.rsi; rdi = regs.rdi;
	rbp = regs.rbp; r8  = regs.r8;  r9  = regs.r9;
	r10 = regs.r10; r11 = regs.r11; r12 = regs.r12;
	r13 = regs.r13; r14 = regs.r14; r15 = regs.r15;

	printf("Registers:\n");
	printf(" rip: %016lx   rsp: %016lx flags: %016lx\n", rip, rsp, rflags);
	printf(" rax: %016lx   ebx: %016lx   ecx: %016lx\n", rax, rbx, rcx);
	printf(" rdx: %016lx   rsi: %016lx   rdi: %016lx\n", rdx, rsi, rdi);
	printf(" rbp: %016lx   r8:  %016lx   r9:  %016lx\n", rbp, r8,  r9);
	printf(" r10: %016lx   r11: %016lx   r12: %016lx\n", r10, r11, r12);
	printf(" r13: %016lx   r14: %016lx   r15: %016lx\n", r13, r14, r15);

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
		die("KVM_GET_REGS failed");

	cr0 = sregs.cr0; cr2 = sregs.cr2; cr3 = sregs.cr3;
	cr4 = sregs.cr4; cr8 = sregs.cr8;

	printf("Segment registers:\n");
	printf(" cr0: %016lx   cr2: %016lx   cr3: %016lx\n", cr0, cr2, cr3);
	printf(" cr4: %016lx   cr8: %016lx\n", cr4, cr8);
	printf(" register  selector  base              limit     type  p dpl db s l g\n");
	print_segment("cs", &sregs.cs);
	print_segment("ss", &sregs.ss);
	print_segment("ds", &sregs.ds);
	print_segment("es", &sregs.es);
	print_segment("fs", &sregs.fs);
	print_segment("gs", &sregs.gs);
}

void kvm__show_code(struct kvm *self)
{
	unsigned int code_bytes = 64;
	unsigned int code_prologue = code_bytes * 43 / 64;
	unsigned int code_len = code_bytes;
	unsigned char c;
	unsigned int i;
	uint8_t *ip;

	ip = guest_addr_to_host(self, self->regs.rip - code_prologue);

	printf("Code: ");

	for (i = 0; i < code_len; i++, ip++) {
		c = *ip;

		if (ip == guest_addr_to_host(self, self->regs.rip))
			printf("<%02x> ", c);
		else
			printf("%02x ", c);
	}

	printf("\n");
}
