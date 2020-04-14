#include "kvm/ioport.h"
#include "kvm/irq.h"

int ioport__setup_arch(struct kvm *kvm)
{
	return 0;
}

void ioport__map_irq(u8 *irq)
{
	*irq = irq__alloc_line();
}
