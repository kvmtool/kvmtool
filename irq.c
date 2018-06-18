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

struct msi_routing_ops irq__default_routing_ops;
struct msi_routing_ops *msi_routing_ops = &irq__default_routing_ops;

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

static int irq__update_msix_routes(struct kvm *kvm,
				   struct kvm_irq_routing_entry *entry)
{
	return ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing);
}

static bool irq__default_can_signal_msi(struct kvm *kvm)
{
	return kvm__supports_extension(kvm, KVM_CAP_SIGNAL_MSI);
}

static int irq__default_signal_msi(struct kvm *kvm, struct kvm_msi *msi)
{
	return ioctl(kvm->vm_fd, KVM_SIGNAL_MSI, msi);
}

struct msi_routing_ops irq__default_routing_ops = {
	.update_route	= irq__update_msix_routes,
	.signal_msi	= irq__default_signal_msi,
	.can_signal_msi	= irq__default_can_signal_msi,
};

bool irq__can_signal_msi(struct kvm *kvm)
{
	return msi_routing_ops->can_signal_msi(kvm);
}

int irq__signal_msi(struct kvm *kvm, struct kvm_msi *msi)
{
	return msi_routing_ops->signal_msi(kvm, msi);
}

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg, u32 device_id)
{
	int r;
	struct kvm_irq_routing_entry *entry;

	if (!check_for_irq_routing(kvm))
		return -ENXIO;

	r = irq__allocate_routing_entry();
	if (r)
		return r;

	entry = &irq_routing->entries[irq_routing->nr];
	*entry = (struct kvm_irq_routing_entry) {
		.gsi = next_gsi,
		.type = KVM_IRQ_ROUTING_MSI,
		.u.msi.address_hi = msg->address_hi,
		.u.msi.address_lo = msg->address_lo,
		.u.msi.data = msg->data,
	};

	if (kvm->msix_needs_devid) {
		entry->flags = KVM_MSI_VALID_DEVID;
		entry->u.msi.devid = device_id;
	}

	irq_routing->nr++;

	r = msi_routing_ops->update_route(kvm, entry);
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

	if (msi_routing_ops->update_route(kvm, &irq_routing->entries[i]))
		die_perror("KVM_SET_GSI_ROUTING");
}

int irq__common_add_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd,
			   int resample_fd)
{
	struct kvm_irqfd irqfd = {
		.fd		= trigger_fd,
		.gsi		= gsi,
		.flags		= resample_fd > 0 ? KVM_IRQFD_FLAG_RESAMPLE : 0,
		.resamplefd	= resample_fd,
	};

	/* If we emulate MSI routing, translate the MSI to the corresponding IRQ */
	if (msi_routing_ops->translate_gsi)
		irqfd.gsi = msi_routing_ops->translate_gsi(kvm, gsi);

	return ioctl(kvm->vm_fd, KVM_IRQFD, &irqfd);
}

void irq__common_del_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd)
{
	struct kvm_irqfd irqfd = {
		.fd		= trigger_fd,
		.gsi		= gsi,
		.flags		= KVM_IRQFD_FLAG_DEASSIGN,
	};

	if (msi_routing_ops->translate_gsi)
		irqfd.gsi = msi_routing_ops->translate_gsi(kvm, gsi);

	ioctl(kvm->vm_fd, KVM_IRQFD, &irqfd);
}

int __attribute__((weak)) irq__exit(struct kvm *kvm)
{
	free(irq_routing);
	return 0;
}
dev_base_exit(irq__exit);
