#include "kvm/kvm.h"

#include "kvm/cpufeature.h"
#include "kvm/interrupt.h"
#include "kvm/e820.h"
#include "kvm/util.h"

#include <linux/kvm.h>

#include <asm/bootparam.h>

#include <sys/ioctl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>

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

#define DEFINE_KVM_EXT(ext)		\
	.name = #ext,			\
	.code = ext

struct {
	const char *name;
	int code;
} kvm_req_ext[] = {
	{ DEFINE_KVM_EXT(KVM_CAP_COALESCED_MMIO) },
	{ DEFINE_KVM_EXT(KVM_CAP_SET_TSS_ADDR) },
	{ DEFINE_KVM_EXT(KVM_CAP_PIT2) },
	{ DEFINE_KVM_EXT(KVM_CAP_USER_MEMORY) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQ_ROUTING) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQCHIP) },
	{ DEFINE_KVM_EXT(KVM_CAP_HLT) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQ_INJECT_STATUS) },
	{ DEFINE_KVM_EXT(KVM_CAP_EXT_CPUID) },
};

static bool kvm__supports_extension(struct kvm *self, unsigned int extension)
{
	int ret;

	ret = ioctl(self->sys_fd, KVM_CHECK_EXTENSION, extension);
	if (ret < 0)
		return false;

	return ret;
}

static int kvm__check_extensions(struct kvm *self)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(kvm_req_ext); i++) {
		if (!kvm__supports_extension(self, kvm_req_ext[i].code)) {
			error("Unsuppored KVM extension detected: %s",
				kvm_req_ext[i].name);
			return (int)-i;
		}
	}

	return 0;
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

static bool kvm__cpu_supports_vm(void)
{
	struct cpuid_regs regs;
	uint32_t eax_base;
	int feature;

	regs	= (struct cpuid_regs) {
		.eax		= 0x00,
	};
	host_cpuid(&regs);

	switch (regs.ebx) {
	case CPUID_VENDOR_INTEL_1:
		eax_base	= 0x00;
		feature		= KVM__X86_FEATURE_VMX;
		break;

	case CPUID_VENDOR_AMD_1:
		eax_base	= 0x80000000;
		feature		= KVM__X86_FEATURE_SVM;
		break;

	default:
		return false;
	}

	regs	= (struct cpuid_regs) {
		.eax		= eax_base,
	};
	host_cpuid(&regs);

	if (regs.eax < eax_base + 0x01)
		return false;

	regs	= (struct cpuid_regs) {
		.eax		= eax_base + 0x01
	};
	host_cpuid(&regs);

	return regs.ecx & (1 << feature);
}

struct kvm *kvm__init(const char *kvm_dev, unsigned long ram_size)
{
	struct kvm_userspace_memory_region mem;
	struct kvm_pit_config pit_config = { .flags = 0, };
	struct kvm *self;
	long page_size;
	int mmap_size;
	int ret;

	if (!kvm__cpu_supports_vm())
		die("Your CPU does not support hardware virtualization");

	self = kvm__new();

	self->sys_fd = open(kvm_dev, O_RDWR);
	if (self->sys_fd < 0) {
		if (errno == ENOENT)
			die("'%s' not found. Please make sure your kernel has CONFIG_KVM enabled and that the KVM modules are loaded.", kvm_dev);

		die_perror("open");
	}

	ret = ioctl(self->sys_fd, KVM_GET_API_VERSION, 0);
	if (ret != KVM_API_VERSION)
		die_perror("KVM_API_VERSION ioctl");

	self->vm_fd = ioctl(self->sys_fd, KVM_CREATE_VM, 0);
	if (self->vm_fd < 0)
		die_perror("KVM_CREATE_VM ioctl");

	if (kvm__check_extensions(self))
		die("A required KVM extention is not supported by OS");

	ret = ioctl(self->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000);
	if (ret < 0)
		die_perror("KVM_SET_TSS_ADDR ioctl");

	ret = ioctl(self->vm_fd, KVM_CREATE_PIT2, &pit_config);
	if (ret < 0)
		die_perror("KVM_CREATE_PIT2 ioctl");

	self->ram_size		= ram_size;

	page_size	= sysconf(_SC_PAGESIZE);
	if (posix_memalign(&self->ram_start, page_size, self->ram_size) != 0)
		die("out of memory");

	mem = (struct kvm_userspace_memory_region) {
		.slot			= 0,
		.guest_phys_addr	= 0x0UL,
		.memory_size		= self->ram_size,
		.userspace_addr		= (unsigned long) self->ram_start,
	};

	ret = ioctl(self->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
	if (ret < 0)
		die_perror("KVM_SET_USER_MEMORY_REGION ioctl");

	ret = ioctl(self->vm_fd, KVM_CREATE_IRQCHIP);
	if (ret < 0)
		die_perror("KVM_CREATE_IRQCHIP ioctl");

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
#define BOOT_CMDLINE_OFFSET	0x20000

#define BOOT_PROTOCOL_REQUIRED	0x206
#define LOAD_HIGH		0x01

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
#define INITRD_START			0x1000000UL
#define BZ_DEFAULT_SETUP_SECTS		4
static const char *BZIMAGE_MAGIC	= "HdrS";

static bool load_bzimage(struct kvm *self, int fd_kernel,
			int fd_initrd, const char *kernel_cmdline)
{
	struct boot_params *kern_boot;
	unsigned long setup_sects;
	struct boot_params boot;
	size_t cmdline_size;
	ssize_t setup_size;
	void *p;
	int nr;

	/*
	 * See Documentation/x86/boot.txt for details no bzImage on-disk and
	 * memory layout.
	 */

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	if (read(fd_kernel, &boot, sizeof(boot)) != sizeof(boot))
		return false;

        if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)))
		return false;

	if (boot.hdr.version < BOOT_PROTOCOL_REQUIRED) {
		die("Too old kernel");
	}

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	if (!boot.hdr.setup_sects)
		boot.hdr.setup_sects = BZ_DEFAULT_SETUP_SECTS;
	setup_sects = boot.hdr.setup_sects + 1;

	setup_size = setup_sects << 9;
	p = guest_real_to_host(self, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);

	/* copy setup.bin to mem*/
	if (read(fd_kernel, p, setup_size) != setup_size)
		die_perror("read");

	/* copy vmlinux.bin to BZ_KERNEL_START*/
	p = guest_flat_to_host(self, BZ_KERNEL_START);

	while ((nr = read(fd_kernel, p, 65536)) > 0)
		p += nr;

	p = guest_flat_to_host(self, BOOT_CMDLINE_OFFSET);
	if (kernel_cmdline) {
		cmdline_size = strlen(kernel_cmdline) + 1;
		if (cmdline_size > boot.hdr.cmdline_size)
			cmdline_size = boot.hdr.cmdline_size;

		memset(p, 0, boot.hdr.cmdline_size);
		memcpy(p, kernel_cmdline, cmdline_size - 1);
	}

	kern_boot	= guest_real_to_host(self, BOOT_LOADER_SELECTOR, 0x00);

	kern_boot->hdr.cmd_line_ptr	= BOOT_CMDLINE_OFFSET;
	kern_boot->hdr.type_of_loader	= 0xff;
	kern_boot->hdr.heap_end_ptr	= 0xfe00;
	kern_boot->hdr.loadflags	|= CAN_USE_HEAP;

	/*
	 * Read initrd image into guest memory
	 */
	if (fd_initrd >= 0) {
		struct stat initrd_stat;
		unsigned long addr;

		if (fstat(fd_initrd, &initrd_stat))
			die_perror("fstat");

		addr = boot.hdr.initrd_addr_max & ~0xfffff;
		for (;;) {
			if (addr < BZ_KERNEL_START)
				die("Not enough memory for initrd");
			else if (addr < (self->ram_size - initrd_stat.st_size))
				break;
			addr -= 0x100000;
		}

		p = guest_flat_to_host(self, addr);
		nr = read(fd_initrd, p, initrd_stat.st_size);
		if (nr != initrd_stat.st_size)
			die("Failed to read initrd");

		kern_boot->hdr.ramdisk_image	= addr;
		kern_boot->hdr.ramdisk_size	= initrd_stat.st_size;
	}

	self->boot_selector	= BOOT_LOADER_SELECTOR;
	/*
	 * The real-mode setup code starts at offset 0x200 of a bzImage. See
	 * Documentation/x86/boot.txt for details.
	 */
	self->boot_ip		= BOOT_LOADER_IP + 0x200;
	self->boot_sp		= BOOT_LOADER_SP;

	return true;
}

bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
		const char *initrd_filename, const char *kernel_cmdline)
{
	bool ret;
	int fd_kernel = -1, fd_initrd = -1;

	fd_kernel = open(kernel_filename, O_RDONLY);
	if (fd_kernel < 0)
		die("Unable to open kernel %s", kernel_filename);

	if (initrd_filename) {
		fd_initrd = open(initrd_filename, O_RDONLY);
		if (fd_initrd < 0)
			die("Unable to open initrd %s", initrd_filename);
	}

	ret = load_bzimage(kvm, fd_kernel, fd_initrd, kernel_cmdline);

	if (initrd_filename)
		close(fd_initrd);

	if (ret)
		goto found_kernel;

	warning("%s is not a bzImage. Trying to load it as a flat binary...", kernel_filename);

	ret = load_flat_binary(kvm, fd_kernel);
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

static inline bool is_in_protected_mode(struct kvm *self)
{
	return self->sregs.cr0 & 0x01;
}

static inline uint64_t ip_to_flat(struct kvm *self, uint64_t ip)
{
	uint64_t cs;

	/*
	 * NOTE! We should take code segment base address into account here.
	 * Luckily it's usually zero because Linux uses flat memory model.
	 */
	if (is_in_protected_mode(self))
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
#ifdef CONFIG_X86_64
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_STAR,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_CSTAR,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_KERNEL_GS_BASE,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_FMASK,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_LSTAR,		0x0);
#endif
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

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &self->sregs) < 0)
		die_perror("KVM_GET_SREGS failed");

	self->sregs.cs.selector	= self->boot_selector;
	self->sregs.cs.base	= selector_to_base(self->boot_selector);
	self->sregs.ss.selector	= self->boot_selector;
	self->sregs.ss.base	= selector_to_base(self->boot_selector);
	self->sregs.ds.selector	= self->boot_selector;
	self->sregs.ds.base	= selector_to_base(self->boot_selector);
	self->sregs.es.selector	= self->boot_selector;
	self->sregs.es.base	= selector_to_base(self->boot_selector);
	self->sregs.fs.selector	= self->boot_selector;
	self->sregs.fs.base	= selector_to_base(self->boot_selector);
	self->sregs.gs.selector	= self->boot_selector;
	self->sregs.gs.base	= selector_to_base(self->boot_selector);

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

void kvm__setup_mem(struct kvm *self)
{
	struct e820_entry *mem_map;
	unsigned char *size;

	size		= guest_flat_to_host(self, E820_MAP_SIZE);
	mem_map		= guest_flat_to_host(self, E820_MAP_START);

	*size		= 4;

	mem_map[0]	= (struct e820_entry) {
		.addr		= REAL_MODE_IVT_BEGIN,
		.size		= EBDA_START - REAL_MODE_IVT_BEGIN,
		.type		= E820_MEM_USABLE,
	};
	mem_map[1]	= (struct e820_entry) {
		.addr		= EBDA_START,
		.size		= VGA_RAM_BEGIN - EBDA_START,
		.type		= E820_MEM_RESERVED,
	};
	mem_map[2]	= (struct e820_entry) {
		.addr		= MB_BIOS_BEGIN,
		.size		= MB_BIOS_END - MB_BIOS_BEGIN,
		.type		= E820_MEM_RESERVED,
	};
	mem_map[3]	= (struct e820_entry) {
		.addr		= BZ_KERNEL_START,
		.size		= self->ram_size - BZ_KERNEL_START,
		.type		= E820_MEM_USABLE,
	};
}

#define TIMER_INTERVAL_NS 1000000	/* 1 msec */

static void alarm_handler(int sig)
{
}

/*
 * This function sets up a timer that's used to inject interrupts from the
 * userspace hypervisor into the guest at periodical intervals. Please note
 * that clock interrupt, for example, is not handled here.
 */
void kvm__start_timer(struct kvm *self)
{
	struct itimerspec its;
	struct sigaction sa;
	struct sigevent sev;

	sigfillset(&sa.sa_mask);
	sa.sa_flags			= 0;
	sa.sa_handler			= alarm_handler;

	sigaction(SIGALRM, &sa, NULL);

	memset(&sev, 0, sizeof(struct sigevent));
	sev.sigev_value.sival_int	= 0;
	sev.sigev_notify		= SIGEV_SIGNAL;
	sev.sigev_signo			= SIGALRM;

	if (timer_create(CLOCK_REALTIME, &sev, &self->timerid) < 0)
		die("timer_create()");

	its.it_value.tv_sec		= TIMER_INTERVAL_NS / 1000000000;
	its.it_value.tv_nsec		= TIMER_INTERVAL_NS % 1000000000;
	its.it_interval.tv_sec		= its.it_value.tv_sec;
	its.it_interval.tv_nsec		= its.it_value.tv_nsec;

	if (timer_settime(self->timerid, 0, &its, NULL) < 0)
		die("timer_settime()");
}

void kvm__run(struct kvm *self)
{
	int err;

	err = ioctl(self->vcpu_fd, KVM_RUN, 0);
	if (err && (errno != EINTR && errno != EAGAIN))
		die_perror("KVM_RUN failed");
}

void kvm__irq_line(struct kvm *self, int irq, int level)
{
	struct kvm_irq_level irq_level;

	irq_level	= (struct kvm_irq_level) {
		{
			.irq		= irq,
		},
		.level		= level,
	};

	if (ioctl(self->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
		die_perror("KVM_IRQ_LINE failed");
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
	printf(" [ efer: %016" PRIx64 "  apic base: %016" PRIx64 "  nmi: %s ]\n",
		(uint64_t) sregs.efer, (uint64_t) sregs.apic_base,
		(self->nmi_disabled ? "disabled" : "enabled"));
	printf("Interrupt bitmap:\n");
	printf(" ");
	for (i = 0; i < (KVM_NR_INTERRUPTS + 63) / 64; i++)
		printf("%016" PRIx64 " ", (uint64_t) sregs.interrupt_bitmap[i]);
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
		if (!host_ptr_in_ram(self, ip))
			break;

		c = *ip;

		if (ip == guest_flat_to_host(self, ip_to_flat(self, self->regs.rip)))
			printf("<%02x> ", c);
		else
			printf("%02x ", c);
	}

	printf("\n");

	printf("Stack:\n");
	kvm__dump_mem(self, self->regs.rsp, 32);
}

void kvm__show_page_tables(struct kvm *self)
{
	uint64_t *pte1;
	uint64_t *pte2;
	uint64_t *pte3;
	uint64_t *pte4;

	if (!is_in_protected_mode(self))
		return;

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &self->sregs) < 0)
		die("KVM_GET_SREGS failed");

	pte4	= guest_flat_to_host(self, self->sregs.cr3);
	if (!host_ptr_in_ram(self, pte4))
		return;

	pte3	= guest_flat_to_host(self, (*pte4 & ~0xfff));
	if (!host_ptr_in_ram(self, pte3))
		return;

	pte2	= guest_flat_to_host(self, (*pte3 & ~0xfff));
	if (!host_ptr_in_ram(self, pte2))
		return;

	pte1	= guest_flat_to_host(self, (*pte2 & ~0xfff));
	if (!host_ptr_in_ram(self, pte1))
		return;

	printf("Page Tables:\n");
	if (*pte2 & (1 << 7))
		printf(" pte4: %016" PRIx64 "   pte3: %016" PRIx64
			"   pte2: %016" PRIx64 "\n",
			*pte4, *pte3, *pte2);
	else
		printf(" pte4: %016" PRIx64 "   pte3: %016" PRIx64 "   pte2: %016"
			PRIx64 "   pte1: %016" PRIx64 "\n",
			*pte4, *pte3, *pte2, *pte1);
}

void kvm__dump_mem(struct kvm *self, unsigned long addr, unsigned long size)
{
	unsigned char *p;
	unsigned long n;

	size &= ~7; /* mod 8 */
	if (!size)
		return;

	p = guest_flat_to_host(self, addr);

	for (n = 0; n < size; n+=8) {
		if (!host_ptr_in_ram(self, p + n))
			break;

		printf("  0x%08lx: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			addr + n, p[n + 0], p[n + 1], p[n + 2], p[n + 3],
				  p[n + 4], p[n + 5], p[n + 6], p[n + 7]);
	}
}
