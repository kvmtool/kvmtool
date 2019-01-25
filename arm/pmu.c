#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include "arm-common/gic.h"
#include "arm-common/pmu.h"

#ifdef CONFIG_ARM64
static int set_pmu_attr(struct kvm *kvm, int vcpu_idx,
			struct kvm_device_attr *attr)
{
	int ret, fd;

	fd = kvm->cpus[vcpu_idx]->vcpu_fd;

	ret = ioctl(fd, KVM_HAS_DEVICE_ATTR, attr);
	if (!ret) {
		ret = ioctl(fd, KVM_SET_DEVICE_ATTR, attr);
		if (ret)
			perror("PMU KVM_SET_DEVICE_ATTR failed");
	} else {
		pr_err("Unsupported PMU on vcpu%d\n", vcpu_idx);
	}

	return ret;
}

void pmu__generate_fdt_nodes(void *fdt, struct kvm *kvm)
{
	const char compatible[] = "arm,armv8-pmuv3";
	int irq = KVM_ARM_PMUv3_PPI;
	int i, ret;

	u32 cpu_mask = (((1 << kvm->nrcpus) - 1) << GIC_FDT_IRQ_PPI_CPU_SHIFT) \
		       & GIC_FDT_IRQ_PPI_CPU_MASK;
	u32 irq_prop[] = {
		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(irq - 16),
		cpu_to_fdt32(cpu_mask | IRQ_TYPE_LEVEL_HIGH),
	};

	if (!kvm->cfg.arch.has_pmuv3)
		return;

	if (!kvm__supports_extension(kvm, KVM_CAP_ARM_PMU_V3)) {
		pr_info("PMU unsupported\n");
		return;
	}

	for (i = 0; i < kvm->nrcpus; i++) {
		struct kvm_device_attr pmu_attr;

		pmu_attr = (struct kvm_device_attr){
			.group	= KVM_ARM_VCPU_PMU_V3_CTRL,
			.addr	= (u64)(unsigned long)&irq,
			.attr	= KVM_ARM_VCPU_PMU_V3_IRQ,
		};

		ret = set_pmu_attr(kvm, i, &pmu_attr);
		if (ret)
			return;

		pmu_attr = (struct kvm_device_attr){
			.group	= KVM_ARM_VCPU_PMU_V3_CTRL,
			.attr	= KVM_ARM_VCPU_PMU_V3_INIT,
		};

		ret = set_pmu_attr(kvm, i, &pmu_attr);
		if (ret)
			return;
	}

	_FDT(fdt_begin_node(fdt, "pmu"));
	_FDT(fdt_property(fdt, "compatible", compatible, sizeof(compatible)));
	_FDT(fdt_property(fdt, "interrupts", irq_prop, sizeof(irq_prop)));
	_FDT(fdt_end_node(fdt));
}
#else
void pmu__generate_fdt_nodes(void *fdt, struct kvm *kvm) { }
#endif
