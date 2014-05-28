#include "kvm/irq.h"
#include "kvm/kvm.h"

#include <stdlib.h>

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	pr_warning("irq__add_msix_route");
	return 1;
}
