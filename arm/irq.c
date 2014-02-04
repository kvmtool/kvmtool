#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include "arm-common/gic.h"

int irq__alloc_line(void)
{
	return gic__alloc_irqnum();
}

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	die(__FUNCTION__);
	return 0;
}
