#include "kvm/kvm-cpu.h"
#include "kvm/kvm.h"

#include <asm/ptrace.h>

#define ARM_CORE_REG(x)	(KVM_REG_ARM | KVM_REG_SIZE_U32 | KVM_REG_ARM_CORE | \
			 KVM_REG_ARM_CORE_REG(x))

void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu)
{
	struct kvm *kvm	= vcpu->kvm;
	struct kvm_one_reg reg;
	u32 data;

	/* Who said future-proofing was a good idea? */
	reg.addr = (u64)(unsigned long)&data;

	/* cpsr = IRQs/FIQs masked */
	data	= PSR_I_BIT | PSR_F_BIT | SVC_MODE;
	reg.id	= ARM_CORE_REG(usr_regs.ARM_cpsr);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (cpsr)");

	if (vcpu->cpu_id == 0) {
		/* r0 = 0 */
		data	= 0;
		reg.id	= ARM_CORE_REG(usr_regs.ARM_r0);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (r0)");

		/* r1 = machine type (-1) */
		data	= -1;
		reg.id	= ARM_CORE_REG(usr_regs.ARM_r1);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (r1)");

		/* r2 = physical address of the device tree blob */
		data	= kvm->arch.dtb_guest_start;
		reg.id	= ARM_CORE_REG(usr_regs.ARM_r2);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (r2)");

		/* pc = start of kernel image */
		data	= kvm->arch.kern_guest_start;
		reg.id	= ARM_CORE_REG(usr_regs.ARM_pc);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (pc)");

	} else {
		/* Simply enter the pen */
		data	= kvm->arch.smp_pen_guest_start;
		reg.id	= ARM_CORE_REG(usr_regs.ARM_pc);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (SMP pc)");
	}
}

void kvm_cpu__show_code(struct kvm_cpu *vcpu)
{
	struct kvm_one_reg reg;
	u32 data;

	reg.addr = (u64)(unsigned long)&data;

	printf("*pc:\n");
	reg.id = ARM_CORE_REG(usr_regs.ARM_pc);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (show_code @ PC)");

	kvm__dump_mem(vcpu->kvm, data, 32);
	printf("\n");

	printf("*lr (svc):\n");
	reg.id = ARM_CORE_REG(svc_regs[1]);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (show_code @ LR_svc)");
	data &= ~0x1;

	kvm__dump_mem(vcpu->kvm, data, 32);
	printf("\n");
}

void kvm_cpu__show_registers(struct kvm_cpu *vcpu)
{
	struct kvm_one_reg reg;
	u32 data;
	int debug_fd = kvm_cpu__get_debug_fd();

	reg.addr	= (u64)(unsigned long)&data;
	dprintf(debug_fd, "\n Registers:\n");

	reg.id		= ARM_CORE_REG(usr_regs.ARM_pc);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (pc)");
	dprintf(debug_fd, " PC:    0x%x\n", data);

	reg.id		= ARM_CORE_REG(usr_regs.ARM_cpsr);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (cpsr)");
	dprintf(debug_fd, " CPSR:  0x%x\n", data);

	reg.id		= ARM_CORE_REG(svc_regs[0]);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (SP_svc)");
	dprintf(debug_fd, " SP_svc:  0x%x\n", data);

	reg.id		= ARM_CORE_REG(svc_regs[1]);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (LR_svc)");
	dprintf(debug_fd, " LR_svc:  0x%x\n", data);
}
