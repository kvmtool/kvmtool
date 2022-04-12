#ifndef ARM_COMMON__PMU_H
#define ARM_COMMON__PMU_H

#define KVM_ARM_PMUv3_PPI			23

void pmu__generate_fdt_nodes(void *fdt, struct kvm *kvm);

#endif /* ARM_COMMON__PMU_H */
