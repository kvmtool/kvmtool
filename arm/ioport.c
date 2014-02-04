#include "kvm/ioport.h"
#include "kvm/irq.h"

void ioport__setup_arch(struct kvm *kvm)
{
}

void ioport__map_irq(u8 *irq)
{
	*irq = irq__alloc_line();
}
