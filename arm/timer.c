#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include "arm-common/gic.h"
#include "arm-common/timer.h"

void timer__generate_fdt_nodes(void *fdt, struct kvm *kvm, int *irqs)
{
	const char compatible[] = "arm,armv8-timer\0arm,armv7-timer";

	u32 cpu_mask = (((1 << kvm->nrcpus) - 1) << GIC_FDT_IRQ_PPI_CPU_SHIFT) \
		       & GIC_FDT_IRQ_PPI_CPU_MASK;
	u32 irq_prop[] = {
		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(irqs[0]),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),

		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(irqs[1]),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),

		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(irqs[2]),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),

		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(irqs[3]),
		cpu_to_fdt32(cpu_mask | GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),
	};

	_FDT(fdt_begin_node(fdt, "timer"));
	_FDT(fdt_property(fdt, "compatible", compatible, sizeof(compatible)));
	_FDT(fdt_property(fdt, "interrupts", irq_prop, sizeof(irq_prop)));
	if (kvm->cfg.arch.force_cntfrq > 0)
		_FDT(fdt_property_cell(fdt, "clock-frequency", kvm->cfg.arch.force_cntfrq));
	_FDT(fdt_end_node(fdt));
}

