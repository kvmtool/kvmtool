#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include "arm-common/gic.h"

#include <linux/byteorder.h>
#include <linux/types.h>

static void generate_timer_nodes(void *fdt, struct kvm *kvm)
{
	u32 cpu_mask = (((1 << kvm->nrcpus) - 1) << GIC_FDT_IRQ_PPI_CPU_SHIFT) \
		       & GIC_FDT_IRQ_PPI_CPU_MASK;
	u32 irq_prop[] = {
		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(13),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),

		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(14),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),

		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(11),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),

		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(10),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),
	};

	_FDT(fdt_begin_node(fdt, "timer"));
	_FDT(fdt_property_string(fdt, "compatible", "arm,armv7-timer"));
	_FDT(fdt_property(fdt, "interrupts", irq_prop, sizeof(irq_prop)));
	_FDT(fdt_end_node(fdt));
}

static void generate_fdt_nodes(void *fdt, struct kvm *kvm, u32 gic_phandle)
{
	gic__generate_fdt_nodes(fdt, gic_phandle);
	generate_timer_nodes(fdt, kvm);
}

static int cortex_a15__vcpu_init(struct kvm_cpu *vcpu)
{
	vcpu->generate_fdt_nodes = generate_fdt_nodes;
	return 0;
}

static struct kvm_arm_target target_cortex_a15 = {
	.id		= KVM_ARM_TARGET_CORTEX_A15,
	.compatible	= "arm,cortex-a15",
	.init		= cortex_a15__vcpu_init,
};

static int cortex_a15__core_init(struct kvm *kvm)
{
	return kvm_cpu__register_kvm_arm_target(&target_cortex_a15);
}
core_init(cortex_a15__core_init);
