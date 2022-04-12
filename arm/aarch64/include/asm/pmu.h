#ifndef __ARM_PMU_H__
#define __ARM_PMU_H__

#define KVM_ARM_PMUv3_PPI			23

void pmu__generate_fdt_nodes(void *fdt, struct kvm *kvm);

#endif /* __ARM_PMU_H__ */
