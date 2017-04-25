#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/kvm.h>
#include <errno.h>

#include "kvm/kvm.h"
#include "kvm/irq.h"
#include "kvm/kvm-arch.h"

static u8 next_line = KVM_IRQ_OFFSET;
static int allocated_gsis = 0;

int next_gsi;

struct kvm_irq_routing *irq_routing = NULL;

int irq__alloc_line(void)
{
	return next_line++;
}

int irq__get_nr_allocated_lines(void)
{
	return next_line - KVM_IRQ_OFFSET;
}

int irq__allocate_routing_entry(void)
{
	size_t table_size = sizeof(struct kvm_irq_routing);
	size_t old_size = table_size;
	int nr_entries = 0;

	if (irq_routing)
		nr_entries = irq_routing->nr;

	if (nr_entries < allocated_gsis)
		return 0;

	old_size += sizeof(struct kvm_irq_routing_entry) * allocated_gsis;
	allocated_gsis = ALIGN(nr_entries + 1, 32);
	table_size += sizeof(struct kvm_irq_routing_entry) * allocated_gsis;
	irq_routing = realloc(irq_routing, table_size);

	if (irq_routing == NULL)
		return -ENOMEM;
	memset((void *)irq_routing + old_size, 0, table_size - old_size);

	irq_routing->nr = nr_entries;
	irq_routing->flags = 0;

	return 0;
}

static bool check_for_irq_routing(struct kvm *kvm)
{
	static int has_irq_routing = 0;

	if (has_irq_routing == 0) {
		if (kvm__supports_extension(kvm, KVM_CAP_IRQ_ROUTING))
			has_irq_routing = 1;
		else
			has_irq_routing = -1;
	}

	return has_irq_routing > 0;
}

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg, u32 device_id)
{
	int r;

	if (!check_for_irq_routing(kvm))
		return -ENXIO;

	r = irq__allocate_routing_entry();
	if (r)
		return r;

	irq_routing->entries[irq_routing->nr] =
		(struct kvm_irq_routing_entry) {
			.gsi = next_gsi,
			.type = KVM_IRQ_ROUTING_MSI,
			.u.msi.address_hi = msg->address_hi,
			.u.msi.address_lo = msg->address_lo,
			.u.msi.data = msg->data,
		};

	if (kvm->msix_needs_devid) {
		irq_routing->entries[irq_routing->nr].flags = KVM_MSI_VALID_DEVID;
		irq_routing->entries[irq_routing->nr].u.msi.devid = device_id;
	}
	irq_routing->nr++;

	r = ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing);
	if (r)
		return r;

	return next_gsi++;
}

static bool update_data(u32 *ptr, u32 newdata)
{
	if (*ptr == newdata)
		return false;

	*ptr = newdata;
	return true;
}

void irq__update_msix_route(struct kvm *kvm, u32 gsi, struct msi_msg *msg)
{
	struct kvm_irq_routing_msi *entry;
	unsigned int i;
	bool changed;

	for (i = 0; i < irq_routing->nr; i++)
		if (gsi == irq_routing->entries[i].gsi)
			break;
	if (i == irq_routing->nr)
		return;

	entry = &irq_routing->entries[i].u.msi;

	changed  = update_data(&entry->address_hi, msg->address_hi);
	changed |= update_data(&entry->address_lo, msg->address_lo);
	changed |= update_data(&entry->data, msg->data);

	if (!changed)
		return;

	if (ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing) == -1)
		die_perror("KVM_SET_GSI_ROUTING");
}

int __attribute__((weak)) irq__exit(struct kvm *kvm)
{
	free(irq_routing);
	return 0;
}
dev_base_exit(irq__exit);
