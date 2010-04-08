#include "kvm/kvm.h"

#include "kvm/interrupt.h"
#include "kvm/util.h"

#include <linux/kvm.h>

#include <asm/bootparam.h>

#include <sys/ioctl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
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

static inline uint32_t segment_to_flat(uint16_t selector, uint16_t offset)
{
	return ((uint32_t)selector << 4) + (uint32_t) offset;
}

static inline void *guest_flat_to_host(struct kvm *self, unsigned long offset)
{
	return self->ram_start + offset;
}

static inline void *guest_real_to_host(struct kvm *self, uint16_t selector, uint16_t offset)
{
	unsigned long flat = segment_to_flat(selector, offset);

	return guest_flat_to_host(self, flat);
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

void kvm__delete(struct kvm *self)
{
	free(self->ram_start);
	free(self);
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

#define BOOT_LOADER_SELECTOR	0x1000
#define BOOT_LOADER_IP		0x0000
#define BOOT_LOADER_SP		0x8000

static int load_flat_binary(struct kvm *self, int fd)
{
	void *p;
	int nr;

	if (lseek(fd, 0, SEEK_SET) < 0)
		die_perror("lseek");

	p = guest_real_to_host(self, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);

	while ((nr = read(fd, p, 65536)) > 0)
		p += nr;

	self->boot_selector	= BOOT_LOADER_SELECTOR;
	self->boot_ip		= BOOT_LOADER_IP;
	self->boot_sp		= BOOT_LOADER_SP;

	return true;
}

/*
 * The protected mode kernel part of a modern bzImage is loaded at 1 MB by
 * default.
 */
#define BZ_KERNEL_START			0x100000UL

static const char *BZIMAGE_MAGIC	= "HdrS";

#define BZ_DEFAULT_SETUP_SECTS		4

static bool load_bzimage(struct kvm *self, int fd, const char *kernel_cmdline)
{
	struct real_intr_desc intr;
	struct boot_params boot;
	unsigned long setup_sects;
	unsigned int intr_addr;
	size_t cmdline_size, cmdline_offset;
	ssize_t setup_size;
	void *p;
	int nr;

	/*
	 * See Documentation/x86/boot.txt for details no bzImage on-disk and
	 * memory layout.
	 */

	if (lseek(fd, 0, SEEK_SET) < 0)
		die_perror("lseek");

	read(fd, &boot, sizeof(boot));

        if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)) != 0)
		return false;

	if (boot.hdr.version < 0x0200) {
		warning("Too old kernel");
		return false;
	}

	if (lseek(fd, 0, SEEK_SET) < 0)
		die_perror("lseek");

	if (!boot.hdr.setup_sects)
		boot.hdr.setup_sects = BZ_DEFAULT_SETUP_SECTS;
	setup_sects = boot.hdr.setup_sects + 1;

	setup_size = setup_sects << 9;
	p = guest_real_to_host(self, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);

	if (read(fd, p, setup_size) != setup_size)
		die_perror("read");

	p = guest_flat_to_host(self, BZ_KERNEL_START);

	while ((nr = read(fd, p, 65536)) > 0)
		p += nr;

	if (boot.hdr.version < 0x0206)
		boot.hdr.cmdline_size = 256;

	if (kernel_cmdline) {
		cmdline_size = strlen(kernel_cmdline) + 1;
		if (cmdline_size > boot.hdr.cmdline_size)
			cmdline_size = boot.hdr.cmdline_size;
	} else
		cmdline_size = 0;
		

	if (boot.hdr.version < 0x0202 || !(boot.hdr.loadflags & 0x01))
		cmdline_offset = (0x9ff0 - cmdline_size) & ~15;
	else
		cmdline_offset = 0x10000;

	if (kernel_cmdline) {
		p = guest_flat_to_host(self, cmdline_offset);
		memset(p, 0, cmdline_size);
		strcpy(p, kernel_cmdline);
	}

	if (boot.hdr.version >= 0x0200) {
		if (boot.hdr.version >= 0x0202) {
			boot.hdr.cmd_line_ptr =
				(BOOT_LOADER_SELECTOR << 4) + cmdline_offset;
		} else if (boot.hdr.version >= 0x0201) {
			boot.hdr.heap_end_ptr = cmdline_offset - 0x0200;
			boot.hdr.loadflags |= CAN_USE_HEAP;
		}

	}

	self->boot_selector	= BOOT_LOADER_SELECTOR;
	/*
	 * The real-mode setup code starts at offset 0x200 of a bzImage. See
	 * Documentation/x86/boot.txt for details.
	 */
	self->boot_ip		= BOOT_LOADER_IP + 0x200;
	self->boot_sp		= BOOT_LOADER_SP;

	/*
	 * Setup a *fake* real mode vector table, it has only
	 * one real hadler which does just iret
	 *
	 * This is where the BIOS lives -- BDA area
	 */
	intr_addr = BIOS_INTR_NEXT(BDA_START + 0, 16);
	p = guest_flat_to_host(self, intr_addr);
	memcpy(p, intfake, intfake_size);
	intr = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(intr_addr),
		.offset		= 0,
	};
	interrupt_table__setup(&self->interrupt_table, &intr);

	intr_addr = BIOS_INTR_NEXT(BDA_START + intfake_size, 16);
	p = guest_flat_to_host(self, intr_addr);
	memcpy(p, int10, int10_size);
	intr = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(intr_addr),
		.offset		= 0,
	};
	interrupt_table__set(&self->interrupt_table, &intr, 0x10);

	p = guest_flat_to_host(self, 0);
	interrupt_table__copy(&self->interrupt_table, p, REAL_INTR_SIZE);

	return true;
}

bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
			const char *kernel_cmdline)
{
	bool ret;
	int fd;

	fd = open(kernel_filename, O_RDONLY);
	if (fd < 0)
		die("unable to open kernel");

	ret = load_bzimage(kvm, fd, kernel_cmdline);
	if (ret)
		goto found_kernel;

	ret = load_flat_binary(kvm, fd);
	if (ret)
		goto found_kernel;

	die("%s is not a valid bzImage or flat binary", kernel_filename);

found_kernel:
	return ret;
}

static inline uint64_t ip_flat_to_real(struct kvm *self, uint64_t ip)
{
	uint64_t cs = self->sregs.cs.selector;

	return ip - (cs << 4);
}

static inline uint64_t ip_to_flat(struct kvm *self, uint64_t ip)
{
	uint64_t cs;

	/*
	 * NOTE! We should take code segment base address into account here.
	 * Luckily it's usually zero because Linux uses flat memory model.
	 */
	if (self->sregs.cr0 & 0x01)
		return ip;

	cs = self->sregs.cs.selector;

	return ip + (cs << 4);
}

static inline uint32_t selector_to_base(uint16_t selector)
{
	/*
	 * KVM on Intel requires 'base' to be 'selector * 16' in real mode.
	 */
	return (uint32_t)selector * 16;
}

static struct kvm_msrs *kvm_msrs__new(size_t nmsrs)
{
	struct kvm_msrs *self = calloc(1, sizeof(*self) + (sizeof(struct kvm_msr_entry) * nmsrs));

	if (!self)
		die("out of memory");

	return self;
}

#define MSR_IA32_TIME_STAMP_COUNTER	0x10

#define MSR_IA32_SYSENTER_CS		0x174
#define MSR_IA32_SYSENTER_ESP		0x175
#define MSR_IA32_SYSENTER_EIP		0x176

#define MSR_IA32_STAR			0xc0000081
#define MSR_IA32_LSTAR			0xc0000082
#define MSR_IA32_CSTAR			0xc0000083
#define MSR_IA32_FMASK			0xc0000084
#define MSR_IA32_KERNEL_GS_BASE		0xc0000102

#define KVM_MSR_ENTRY(_index, _data)	\
	(struct kvm_msr_entry) { .index = _index, .data = _data }

static void kvm__setup_msrs(struct kvm *self)
{
	unsigned long ndx = 0;

	self->msrs = kvm_msrs__new(100);

	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_SYSENTER_CS,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_SYSENTER_ESP,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_SYSENTER_EIP,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_STAR,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_CSTAR,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_KERNEL_GS_BASE,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_FMASK,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_LSTAR,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_TIME_STAMP_COUNTER,	0x0);

	self->msrs->nmsrs	= ndx;

	if (ioctl(self->vcpu_fd, KVM_SET_MSRS, self->msrs) < 0)
		die_perror("KVM_SET_MSRS failed");
}

static void kvm__setup_fpu(struct kvm *self)
{
	self->fpu = (struct kvm_fpu) {
		.fcw		= 0x37f,
		.mxcsr		= 0x1f80,
	};

	if (ioctl(self->vcpu_fd, KVM_SET_FPU, &self->fpu) < 0)
		die_perror("KVM_SET_FPU failed");
}

static void kvm__setup_regs(struct kvm *self)
{
	self->regs = (struct kvm_regs) {
		/* We start the guest in 16-bit real mode  */
		.rflags		= 0x0000000000000002ULL,

		.rip		= self->boot_ip,
		.rsp		= self->boot_sp,
		.rbp		= self->boot_sp,
	};

	if (self->regs.rip > USHRT_MAX)
		die("ip 0x%" PRIx64 " is too high for real mode", (uint64_t) self->regs.rip);

	if (ioctl(self->vcpu_fd, KVM_SET_REGS, &self->regs) < 0)
		die_perror("KVM_SET_REGS failed");
}

static void kvm__setup_sregs(struct kvm *self)
{
	self->sregs = (struct kvm_sregs) {
		.cr0		= 0x60000010ULL,
		.cs		= (struct kvm_segment) {
			.selector	= self->boot_selector,
			.base		= selector_to_base(self->boot_selector),
			.limit		= 0xffffU,
			.type		= 0x0bU,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.ss		= (struct kvm_segment) {
			.selector	= self->boot_selector,
			.base		= selector_to_base(self->boot_selector),
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.ds		= (struct kvm_segment) {
			.selector	= self->boot_selector,
			.base		= selector_to_base(self->boot_selector),
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.es		= (struct kvm_segment) {
			.selector	= self->boot_selector,
			.base		= selector_to_base(self->boot_selector),
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.fs		= (struct kvm_segment) {
			.selector	= self->boot_selector,
			.base		= selector_to_base(self->boot_selector),
			.limit		= 0xffffU,
			.type		= 0x03U,
			.present	= 1,
			.dpl		= 0x03,
			.s		= 1,
		},
		.gs		= (struct kvm_segment) {
			.selector	= self->boot_selector,
			.base		= selector_to_base(self->boot_selector),
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
			.type		= 0x02U,
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

void kvm__reset_vcpu(struct kvm *self)
{
	kvm__setup_sregs(self);

	kvm__setup_regs(self);

	kvm__setup_fpu(self);

	kvm__setup_msrs(self);
}

void kvm__run(struct kvm *self)
{
	if (ioctl(self->vcpu_fd, KVM_RUN, 0) < 0)
		die_perror("KVM_RUN failed");
}

static void print_dtable(const char *name, struct kvm_dtable *dtable)
{
	printf(" %s                 %016" PRIx64 "  %08" PRIx16 "\n",
		name, (uint64_t) dtable->base, (uint16_t) dtable->limit);
}

static void print_segment(const char *name, struct kvm_segment *seg)
{
	printf(" %s       %04" PRIx16 "      %016" PRIx64 "  %08" PRIx32 "  %02" PRIx8 "    %x %x   %x  %x %x %x %x\n",
		name, (uint16_t) seg->selector, (uint64_t) seg->base, (uint32_t) seg->limit,
		(uint8_t) seg->type, seg->present, seg->dpl, seg->db, seg->s, seg->l, seg->g, seg->avl);
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
	int i;

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
	printf(" rax: %016lx   rbx: %016lx   rcx: %016lx\n", rax, rbx, rcx);
	printf(" rdx: %016lx   rsi: %016lx   rdi: %016lx\n", rdx, rsi, rdi);
	printf(" rbp: %016lx   r8:  %016lx   r9:  %016lx\n", rbp, r8,  r9);
	printf(" r10: %016lx   r11: %016lx   r12: %016lx\n", r10, r11, r12);
	printf(" r13: %016lx   r14: %016lx   r15: %016lx\n", r13, r14, r15);

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
		die("KVM_GET_REGS failed");

	cr0 = sregs.cr0; cr2 = sregs.cr2; cr3 = sregs.cr3;
	cr4 = sregs.cr4; cr8 = sregs.cr8;

	printf(" cr0: %016lx   cr2: %016lx   cr3: %016lx\n", cr0, cr2, cr3);
	printf(" cr4: %016lx   cr8: %016lx\n", cr4, cr8);
	printf("Segment registers:\n");
	printf(" register  selector  base              limit     type  p dpl db s l g avl\n");
	print_segment("cs ", &sregs.cs);
	print_segment("ss ", &sregs.ss);
	print_segment("ds ", &sregs.ds);
	print_segment("es ", &sregs.es);
	print_segment("fs ", &sregs.fs);
	print_segment("gs ", &sregs.gs);
	print_segment("tr ", &sregs.tr);
	print_segment("ldt", &sregs.ldt);
	print_dtable("gdt", &sregs.gdt);
	print_dtable("idt", &sregs.idt);
	printf(" [ efer: %016lx  apic base: %016lx  nmi: %s ]\n", (uint64_t) sregs.efer, (uint64_t) sregs.apic_base,
		(self->nmi_disabled ? "disabled" : "enabled"));
	printf("Interrupt bitmap:\n");
	printf(" ");
	for (i = 0; i < (KVM_NR_INTERRUPTS + 63) / 64; i++)
		printf("%016lx ", (uint64_t) sregs.interrupt_bitmap[i]);
	printf("\n");
}

void kvm__show_code(struct kvm *self)
{
	unsigned int code_bytes = 64;
	unsigned int code_prologue = code_bytes * 43 / 64;
	unsigned int code_len = code_bytes;
	unsigned char c;
	unsigned int i;
	uint8_t *ip;

	if (ioctl(self->vcpu_fd, KVM_GET_REGS, &self->regs) < 0)
		die("KVM_GET_REGS failed");

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &self->sregs) < 0)
		die("KVM_GET_SREGS failed");

	ip = guest_flat_to_host(self, ip_to_flat(self, self->regs.rip) - code_prologue);

	printf("Code: ");

	for (i = 0; i < code_len; i++, ip++) {
		c = *ip;

		if (ip == guest_flat_to_host(self, ip_to_flat(self, self->regs.rip)))
			printf("<%02x> ", c);
		else
			printf("%02x ", c);
	}

	printf("\n");
}

void kvm__dump_mem(struct kvm *self, unsigned long addr, unsigned long size)
{
	unsigned char *p;
	unsigned long n;

	size &= ~7; /* mod 8 */
	if (!size)
		return;

	p = (unsigned char *)guest_flat_to_host(self, addr);

	printf("Guest memory dump:\n");

	for (n = 0; n < size; n+=8)
		printf("0x%08lx: %02x%02x%02x%02x %02x%02x%02x%02x\n",
			addr + n, p[n + 0], p[n + 1], p[n + 2], p[n + 3],
				  p[n + 4], p[n + 5], p[n + 6], p[n + 7]);
}
