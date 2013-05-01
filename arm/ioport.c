#include "kvm/ioport.h"

#include "arm-common/gic.h"

void ioport__setup_arch(struct kvm *kvm)
{
}

void ioport__map_irq(u8 *irq)
{
	*irq = gic__alloc_irqnum();
}
