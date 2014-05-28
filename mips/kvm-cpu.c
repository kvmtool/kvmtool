#include "kvm/kvm-cpu.h"
#include "kvm/term.h"

#include <stdlib.h>

static int debug_fd;

void kvm_cpu__set_debug_fd(int fd)
{
	debug_fd = fd;
}

int kvm_cpu__get_debug_fd(void)
{
	return debug_fd;
}

void kvm_cpu__delete(struct kvm_cpu *vcpu)
{
	free(vcpu);
}

static struct kvm_cpu *kvm_cpu__new(struct kvm *kvm)
{
	struct kvm_cpu *vcpu;

	vcpu = calloc(1, sizeof(*vcpu));
	if (!vcpu)
		return NULL;

	vcpu->kvm = kvm;

	return vcpu;
}

struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id)
{
	struct kvm_cpu *vcpu;
	int mmap_size;
	int coalesced_offset;

	vcpu = kvm_cpu__new(kvm);
	if (!vcpu)
		return NULL;

	vcpu->cpu_id = cpu_id;

	vcpu->vcpu_fd = ioctl(vcpu->kvm->vm_fd, KVM_CREATE_VCPU, cpu_id);
	if (vcpu->vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU ioctl");

	mmap_size = ioctl(vcpu->kvm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

	vcpu->kvm_run = mmap(NULL, mmap_size, PROT_RW, MAP_SHARED, vcpu->vcpu_fd, 0);
	if (vcpu->kvm_run == MAP_FAILED)
		die("unable to mmap vcpu fd");

	vcpu->is_running = true;

	coalesced_offset = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_COALESCED_MMIO);
	if (coalesced_offset)
		vcpu->ring = (void *)vcpu->kvm_run + (coalesced_offset * PAGE_SIZE);

	return vcpu;
}

static void kvm_cpu__setup_regs(struct kvm_cpu *vcpu)
{
	uint32_t v;
	struct kvm_one_reg one_reg;

	memset(&vcpu->regs, 0, sizeof(vcpu->regs));
	vcpu->regs.pc = vcpu->kvm->arch.entry_point;
	vcpu->regs.gpr[4] = vcpu->kvm->arch.argc;
	vcpu->regs.gpr[5] = vcpu->kvm->arch.argv;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &vcpu->regs) < 0)
		die_perror("KVM_SET_REGS failed");


	one_reg.id = KVM_REG_MIPS | KVM_REG_SIZE_U32 | (0x10000 + 8 * 12 + 0); /* Status */
	one_reg.addr = (unsigned long)(uint32_t *)&v;
	v = 6;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &one_reg) < 0)
		die_perror("KVM_SET_ONE_REG failed");
}

/**
 * kvm_cpu__reset_vcpu - reset virtual CPU to a known state
 */
void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu)
{
	kvm_cpu__setup_regs(vcpu);
}

static bool kvm_cpu__hypercall_write_cons(struct kvm_cpu *vcpu)
{
	int term = (int)vcpu->kvm_run->hypercall.args[0];
	u64 addr = vcpu->kvm_run->hypercall.args[1];
	int len = (int)vcpu->kvm_run->hypercall.args[2];
	char *host_addr;

	if (term < 0 || term >= TERM_MAX_DEVS) {
		pr_warning("hypercall_write_cons term out of range <%d>", term);
		return false;
	}

	if ((addr & 0xffffffffc0000000ull) == 0xffffffff80000000ull)
		addr &= 0x1ffffffful; /* Convert KSEG{0,1} to physical. */
	if ((addr & 0xc000000000000000ull) == 0x8000000000000000ull)
		addr &= 0x07ffffffffffffffull; /* Convert XKPHYS to pysical */

	host_addr = guest_flat_to_host(vcpu->kvm, addr);
	if (!host_addr) {
		pr_warning("hypercall_write_cons unmapped physaddr %llx", (unsigned long long)addr);
		return false;
	}

	if ((len <= 0) || !host_ptr_in_ram(vcpu->kvm, host_addr + len)) {
		pr_warning("hypercall_write_cons len out of range <%d>", len);
		return false;
	}

	term_putc(host_addr, len, term);

	return true;
}

#define KVM_HC_MIPS_CONSOLE_OUTPUT 8
bool kvm_cpu__handle_exit(struct kvm_cpu *vcpu)
{
	switch(vcpu->kvm_run->exit_reason) {
	case KVM_EXIT_HYPERCALL:
		if (vcpu->kvm_run->hypercall.nr == KVM_HC_MIPS_CONSOLE_OUTPUT) {
			return kvm_cpu__hypercall_write_cons(vcpu);
		} else {
			pr_warning("KVM_EXIT_HYPERCALL unrecognized call %llu",
				   (unsigned long long)vcpu->kvm_run->hypercall.nr);
			return false;
		}
	case KVM_EXIT_EXCEPTION:
	case KVM_EXIT_INTERNAL_ERROR:
		return false;
	default:
		break;
	}
	return false;
}

void kvm_cpu__arch_nmi(struct kvm_cpu *cpu)
{
}

void kvm_cpu__show_registers(struct kvm_cpu *vcpu)
{
	struct kvm_regs regs;

	if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
		die("KVM_GET_REGS failed");
	dprintf(debug_fd, "\n Registers:\n");
	dprintf(debug_fd,   " ----------\n");
	dprintf(debug_fd, "$0   : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[0],
		(unsigned long long)regs.gpr[1],
		(unsigned long long)regs.gpr[2],
		(unsigned long long)regs.gpr[3]);
	dprintf(debug_fd, "$4   : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[4],
		(unsigned long long)regs.gpr[5],
		(unsigned long long)regs.gpr[6],
		(unsigned long long)regs.gpr[7]);
	dprintf(debug_fd, "$8   : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[8],
		(unsigned long long)regs.gpr[9],
		(unsigned long long)regs.gpr[10],
		(unsigned long long)regs.gpr[11]);
	dprintf(debug_fd, "$12  : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[12],
		(unsigned long long)regs.gpr[13],
		(unsigned long long)regs.gpr[14],
		(unsigned long long)regs.gpr[15]);
	dprintf(debug_fd, "$16  : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[16],
		(unsigned long long)regs.gpr[17],
		(unsigned long long)regs.gpr[18],
		(unsigned long long)regs.gpr[19]);
	dprintf(debug_fd, "$20  : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[20],
		(unsigned long long)regs.gpr[21],
		(unsigned long long)regs.gpr[22],
		(unsigned long long)regs.gpr[23]);
	dprintf(debug_fd, "$24  : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[24],
		(unsigned long long)regs.gpr[25],
		(unsigned long long)regs.gpr[26],
		(unsigned long long)regs.gpr[27]);
	dprintf(debug_fd, "$28  : %016llx %016llx %016llx %016llx\n",
		(unsigned long long)regs.gpr[28],
		(unsigned long long)regs.gpr[29],
		(unsigned long long)regs.gpr[30],
		(unsigned long long)regs.gpr[31]);

	dprintf(debug_fd, "hi   : %016llx\n", (unsigned long long)regs.hi);
	dprintf(debug_fd, "lo   : %016llx\n", (unsigned long long)regs.lo);
	dprintf(debug_fd, "epc  : %016llx\n", (unsigned long long)regs.pc);

	dprintf(debug_fd, "\n");
}

void kvm_cpu__show_code(struct kvm_cpu *vcpu)
{
}

void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu)
{
}
