#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include "arm-common/gic.h"

int irq__register_device(u32 dev, u8 *pin, u8 *line)
{
	*line = gic__alloc_irqnum();
	return 0;
}

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	die(__FUNCTION__);
	return 0;
}
