#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/irq.h"

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	plic__irq_trig(kvm, irq, level, false);
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	plic__irq_trig(kvm, irq, 1, true);
}
