#ifndef ARM_COMMON__TIMER_H
#define ARM_COMMON__TIMER_H

void timer__generate_fdt_nodes(void *fdt, struct kvm *kvm, int *irqs);

#endif /* ARM_COMMON__TIMER_H */
