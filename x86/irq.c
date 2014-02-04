#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

#include <stddef.h>
#include <stdlib.h>

#define IRQ_MAX_GSI			64
#define IRQCHIP_MASTER			0
#define IRQCHIP_SLAVE			1
#define IRQCHIP_IOAPIC			2

/* First 24 GSIs are routed between IRQCHIPs and IOAPICs */
static u32 gsi = 24;

struct kvm_irq_routing *irq_routing;

static int irq__add_routing(u32 gsi, u32 type, u32 irqchip, u32 pin)
{
	if (gsi >= IRQ_MAX_GSI)
		return -ENOSPC;

	irq_routing->entries[irq_routing->nr++] =
		(struct kvm_irq_routing_entry) {
			.gsi = gsi,
			.type = type,
			.u.irqchip.irqchip = irqchip,
			.u.irqchip.pin = pin,
		};

	return 0;
}

int irq__init(struct kvm *kvm)
{
	int i, r;

	irq_routing = calloc(sizeof(struct kvm_irq_routing) +
			IRQ_MAX_GSI * sizeof(struct kvm_irq_routing_entry), 1);
	if (irq_routing == NULL)
		return -ENOMEM;

	/* Hook first 8 GSIs to master IRQCHIP */
	for (i = 0; i < 8; i++)
		if (i != 2)
			irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_MASTER, i);

	/* Hook next 8 GSIs to slave IRQCHIP */
	for (i = 8; i < 16; i++)
		irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_SLAVE, i - 8);

	/* Last but not least, IOAPIC */
	for (i = 0; i < 24; i++) {
		if (i == 0)
			irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_IOAPIC, 2);
		else if (i != 2)
			irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_IOAPIC, i);
	}

	r = ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing);
	if (r) {
		free(irq_routing);
		return errno;
	}

	return 0;
}
dev_base_init(irq__init);

int irq__exit(struct kvm *kvm)
{
	free(irq_routing);
	return 0;
}
dev_base_exit(irq__exit);

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	int r;

	irq_routing->entries[irq_routing->nr++] =
		(struct kvm_irq_routing_entry) {
			.gsi = gsi,
			.type = KVM_IRQ_ROUTING_MSI,
			.u.msi.address_hi = msg->address_hi,
			.u.msi.address_lo = msg->address_lo,
			.u.msi.data = msg->data,
		};

	r = ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing);
	if (r)
		return r;

	return gsi++;
}
