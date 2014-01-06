#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include "arm-common/gic.h"
#include "arm-common/timer.h"

#include <linux/byteorder.h>
#include <linux/types.h>

static void generate_fdt_nodes(void *fdt, struct kvm *kvm, u32 gic_phandle)
{
	int timer_interrupts[4] = {13, 14, 11, 10};

	gic__generate_fdt_nodes(fdt, gic_phandle);
	timer__generate_fdt_nodes(fdt, kvm, timer_interrupts);
}

static int arm_cpu__vcpu_init(struct kvm_cpu *vcpu)
{
	vcpu->generate_fdt_nodes = generate_fdt_nodes;
	return 0;
}

static struct kvm_arm_target target_cortex_a15 = {
	.id		= KVM_ARM_TARGET_CORTEX_A15,
	.compatible	= "arm,cortex-a15",
	.init		= arm_cpu__vcpu_init,
};

static struct kvm_arm_target target_cortex_a7 = {
	.id		= KVM_ARM_TARGET_CORTEX_A7,
	.compatible	= "arm,cortex-a7",
	.init		= arm_cpu__vcpu_init,
};

static int arm_cpu__core_init(struct kvm *kvm)
{
	return (kvm_cpu__register_kvm_arm_target(&target_cortex_a15) ||
		kvm_cpu__register_kvm_arm_target(&target_cortex_a7));
}
core_init(arm_cpu__core_init);
