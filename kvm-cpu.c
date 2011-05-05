#include "kvm/kvm-cpu.h"

#include "kvm/util.h"
#include "kvm/kvm.h"

#include <asm/msr-index.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

static inline bool is_in_protected_mode(struct kvm_cpu *self)
{
	return self->sregs.cr0 & 0x01;
}

static inline u64 ip_to_flat(struct kvm_cpu *self, u64 ip)
{
	u64 cs;

	/*
	 * NOTE! We should take code segment base address into account here.
	 * Luckily it's usually zero because Linux uses flat memory model.
	 */
	if (is_in_protected_mode(self))
		return ip;

	cs = self->sregs.cs.selector;

	return ip + (cs << 4);
}

static inline u32 selector_to_base(u16 selector)
{
	/*
	 * KVM on Intel requires 'base' to be 'selector * 16' in real mode.
	 */
	return (u32)selector * 16;
}

static struct kvm_cpu *kvm_cpu__new(struct kvm *kvm)
{
	struct kvm_cpu *self;

	self		= calloc(1, sizeof *self);
	if (!self)
		return NULL;

	self->kvm	= kvm;

	return self;
}

void kvm_cpu__delete(struct kvm_cpu *self)
{
	if (self->msrs)
		free(self->msrs);

	free(self);
}

struct kvm_cpu *kvm_cpu__init(struct kvm *kvm, unsigned long cpu_id)
{
	struct kvm_cpu *self;
	int mmap_size;

	self		= kvm_cpu__new(kvm);
	if (!self)
		return NULL;

	self->cpu_id	= cpu_id;

	self->vcpu_fd = ioctl(self->kvm->vm_fd, KVM_CREATE_VCPU, cpu_id);
	if (self->vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU ioctl");

	mmap_size = ioctl(self->kvm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

	self->kvm_run = mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, self->vcpu_fd, 0);
	if (self->kvm_run == MAP_FAILED)
		die("unable to mmap vcpu fd");

	return self;
}

void kvm_cpu__enable_singlestep(struct kvm_cpu *self)
{
	struct kvm_guest_debug debug = {
		.control	= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP,
	};

	if (ioctl(self->vcpu_fd, KVM_SET_GUEST_DEBUG, &debug) < 0)
		warning("KVM_SET_GUEST_DEBUG failed");
}

static struct kvm_msrs *kvm_msrs__new(size_t nmsrs)
{
	struct kvm_msrs *self = calloc(1, sizeof(*self) + (sizeof(struct kvm_msr_entry) * nmsrs));

	if (!self)
		die("out of memory");

	return self;
}

#define KVM_MSR_ENTRY(_index, _data)	\
	(struct kvm_msr_entry) { .index = _index, .data = _data }

static void kvm_cpu__setup_msrs(struct kvm_cpu *self)
{
	unsigned long ndx = 0;

	self->msrs = kvm_msrs__new(100);

	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_SYSENTER_CS,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_SYSENTER_ESP,	0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_SYSENTER_EIP,	0x0);
#ifdef CONFIG_X86_64
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_STAR,			0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_CSTAR,			0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_KERNEL_GS_BASE,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_SYSCALL_MASK,		0x0);
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_LSTAR,			0x0);
#endif
	self->msrs->entries[ndx++] = KVM_MSR_ENTRY(MSR_IA32_TSC,		0x0);

	self->msrs->nmsrs	= ndx;

	if (ioctl(self->vcpu_fd, KVM_SET_MSRS, self->msrs) < 0)
		die_perror("KVM_SET_MSRS failed");
}

static void kvm_cpu__setup_fpu(struct kvm_cpu *self)
{
	self->fpu = (struct kvm_fpu) {
		.fcw		= 0x37f,
		.mxcsr		= 0x1f80,
	};

	if (ioctl(self->vcpu_fd, KVM_SET_FPU, &self->fpu) < 0)
		die_perror("KVM_SET_FPU failed");
}

static void kvm_cpu__setup_regs(struct kvm_cpu *self)
{
	self->regs = (struct kvm_regs) {
		/* We start the guest in 16-bit real mode  */
		.rflags		= 0x0000000000000002ULL,

		.rip		= self->kvm->boot_ip,
		.rsp		= self->kvm->boot_sp,
		.rbp		= self->kvm->boot_sp,
	};

	if (self->regs.rip > USHRT_MAX)
		die("ip 0x%llx is too high for real mode", (u64) self->regs.rip);

	if (ioctl(self->vcpu_fd, KVM_SET_REGS, &self->regs) < 0)
		die_perror("KVM_SET_REGS failed");
}

static void kvm_cpu__setup_sregs(struct kvm_cpu *self)
{

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &self->sregs) < 0)
		die_perror("KVM_GET_SREGS failed");

	self->sregs.cs.selector	= self->kvm->boot_selector;
	self->sregs.cs.base	= selector_to_base(self->kvm->boot_selector);
	self->sregs.ss.selector	= self->kvm->boot_selector;
	self->sregs.ss.base	= selector_to_base(self->kvm->boot_selector);
	self->sregs.ds.selector	= self->kvm->boot_selector;
	self->sregs.ds.base	= selector_to_base(self->kvm->boot_selector);
	self->sregs.es.selector	= self->kvm->boot_selector;
	self->sregs.es.base	= selector_to_base(self->kvm->boot_selector);
	self->sregs.fs.selector	= self->kvm->boot_selector;
	self->sregs.fs.base	= selector_to_base(self->kvm->boot_selector);
	self->sregs.gs.selector	= self->kvm->boot_selector;
	self->sregs.gs.base	= selector_to_base(self->kvm->boot_selector);

	if (ioctl(self->vcpu_fd, KVM_SET_SREGS, &self->sregs) < 0)
		die_perror("KVM_SET_SREGS failed");
}

/**
 * kvm_cpu__reset_vcpu - reset virtual CPU to a known state
 */
void kvm_cpu__reset_vcpu(struct kvm_cpu *self)
{
	kvm_cpu__setup_sregs(self);
	kvm_cpu__setup_regs(self);
	kvm_cpu__setup_fpu(self);
	kvm_cpu__setup_msrs(self);
}

static void print_dtable(const char *name, struct kvm_dtable *dtable)
{
	printf(" %s                 %016llx %08hx\n",
		name, (u64) dtable->base, (u16) dtable->limit);
}

static void print_segment(const char *name, struct kvm_segment *seg)
{
	printf(" %s       %04hx      %016llx  %08x  %02hhx    %x %x   %x  %x %x %x %x\n",
		name, (u16) seg->selector, (u64) seg->base, (u32) seg->limit,
		(u8) seg->type, seg->present, seg->dpl, seg->db, seg->s, seg->l, seg->g, seg->avl);
}

void kvm_cpu__show_registers(struct kvm_cpu *self)
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
	printf(" [ efer: %016llx  apic base: %016llx  nmi: %s ]\n",
		(u64) sregs.efer, (u64) sregs.apic_base,
		(self->kvm->nmi_disabled ? "disabled" : "enabled"));
	printf("Interrupt bitmap:\n");
	printf(" ");
	for (i = 0; i < (KVM_NR_INTERRUPTS + 63) / 64; i++)
		printf("%016llx ", (u64) sregs.interrupt_bitmap[i]);
	printf("\n");
}

void kvm_cpu__show_code(struct kvm_cpu *self)
{
	unsigned int code_bytes = 64;
	unsigned int code_prologue = code_bytes * 43 / 64;
	unsigned int code_len = code_bytes;
	unsigned char c;
	unsigned int i;
	u8 *ip;

	if (ioctl(self->vcpu_fd, KVM_GET_REGS, &self->regs) < 0)
		die("KVM_GET_REGS failed");

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &self->sregs) < 0)
		die("KVM_GET_SREGS failed");

	ip = guest_flat_to_host(self->kvm, ip_to_flat(self, self->regs.rip) - code_prologue);

	printf("Code: ");

	for (i = 0; i < code_len; i++, ip++) {
		if (!host_ptr_in_ram(self->kvm, ip))
			break;

		c = *ip;

		if (ip == guest_flat_to_host(self->kvm, ip_to_flat(self, self->regs.rip)))
			printf("<%02x> ", c);
		else
			printf("%02x ", c);
	}

	printf("\n");

	printf("Stack:\n");
	kvm__dump_mem(self->kvm, self->regs.rsp, 32);
}

void kvm_cpu__show_page_tables(struct kvm_cpu *self)
{
	u64 *pte1;
	u64 *pte2;
	u64 *pte3;
	u64 *pte4;

	if (!is_in_protected_mode(self))
		return;

	if (ioctl(self->vcpu_fd, KVM_GET_SREGS, &self->sregs) < 0)
		die("KVM_GET_SREGS failed");

	pte4	= guest_flat_to_host(self->kvm, self->sregs.cr3);
	if (!host_ptr_in_ram(self->kvm, pte4))
		return;

	pte3	= guest_flat_to_host(self->kvm, (*pte4 & ~0xfff));
	if (!host_ptr_in_ram(self->kvm, pte3))
		return;

	pte2	= guest_flat_to_host(self->kvm, (*pte3 & ~0xfff));
	if (!host_ptr_in_ram(self->kvm, pte2))
		return;

	pte1	= guest_flat_to_host(self->kvm, (*pte2 & ~0xfff));
	if (!host_ptr_in_ram(self->kvm, pte1))
		return;

	printf("Page Tables:\n");
	if (*pte2 & (1 << 7))
		printf(" pte4: %016llx   pte3: %016llx"
			"   pte2: %016llx\n",
			*pte4, *pte3, *pte2);
	else
		printf(" pte4: %016llx  pte3: %016llx   pte2: %016"
			"llx   pte1: %016llx\n",
			*pte4, *pte3, *pte2, *pte1);
}

void kvm_cpu__run(struct kvm_cpu *self)
{
	int err;

	err = ioctl(self->vcpu_fd, KVM_RUN, 0);
	if (err && (errno != EINTR && errno != EAGAIN))
		die_perror("KVM_RUN failed");
}

int kvm_cpu__start(struct kvm_cpu *cpu)
{
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);

	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	kvm_cpu__setup_cpuid(cpu);
	kvm_cpu__reset_vcpu(cpu);

	for (;;) {
		kvm_cpu__run(cpu);

		switch (cpu->kvm_run->exit_reason) {
		case KVM_EXIT_DEBUG:
			kvm_cpu__show_registers(cpu);
			kvm_cpu__show_code(cpu);
			break;
		case KVM_EXIT_IO: {
			bool ret;

			ret = kvm__emulate_io(cpu->kvm,
					cpu->kvm_run->io.port,
					(u8 *)cpu->kvm_run +
					cpu->kvm_run->io.data_offset,
					cpu->kvm_run->io.direction,
					cpu->kvm_run->io.size,
					cpu->kvm_run->io.count);

			if (!ret)
				goto panic_kvm;
			break;
		}
		case KVM_EXIT_MMIO: {
			bool ret;

			ret = kvm__emulate_mmio(cpu->kvm,
					cpu->kvm_run->mmio.phys_addr,
					cpu->kvm_run->mmio.data,
					cpu->kvm_run->mmio.len,
					cpu->kvm_run->mmio.is_write);

			if (!ret)
				goto panic_kvm;
			break;
		}
		case KVM_EXIT_INTR:
			break;
		case KVM_EXIT_SHUTDOWN:
			goto exit_kvm;
		default:
			goto panic_kvm;
		}
	}

exit_kvm:
	return 0;

panic_kvm:
	return 1;
}
