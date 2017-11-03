#include <errno.h>
#include <stdlib.h>

#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include "arm-common/gic.h"

#define GICV2M_MSI_TYPER	0x008
#define GICV2M_MSI_SETSPI	0x040
#define GICV2M_MSI_IIDR		0xfcc

#define GICV2M_SPI_MASK		0x3ff
#define GICV2M_MSI_TYPER_VAL(start, nr)	\
	(((start) & GICV2M_SPI_MASK) << 16 | ((nr) & GICV2M_SPI_MASK))

struct gicv2m_chip {
	int	first_spi;
	int	num_spis;
	int	*spis;
	u64	base;
	u64	size;
};

static struct gicv2m_chip v2m;

/*
 * MSI routing is setup lazily, when the guest writes the MSI tables. The guest
 * writes which SPI is associated to an MSI vector into the message data field.
 * The IRQ code notifies us of any change to MSI routing via this callback.
 * Store the MSI->SPI translation for later.
 *
 * Data is the GIC interrupt ID, that includes SGIs and PPIs. SGIs at 0-15, PPIs
 * are 16-31 and SPIs are 32-1019. What we're saving for later is the MSI's GSI
 * number, a logical ID used by KVM for routing. The GSI of an SPI is implicitly
 * defined by KVM to be its pin number (SPI index), and the GSI of an MSI is
 * allocated by kvmtool.
 */
static int gicv2m_update_routing(struct kvm *kvm,
				 struct kvm_irq_routing_entry *entry)
{
	int spi;

	if (entry->type != KVM_IRQ_ROUTING_MSI)
		return -EINVAL;

	if (!entry->u.msi.address_hi && !entry->u.msi.address_lo)
		return 0;

	spi = entry->u.msi.data & GICV2M_SPI_MASK;
	if (spi < v2m.first_spi || spi >= v2m.first_spi + v2m.num_spis) {
		pr_err("invalid SPI number %d", spi);
		return -EINVAL;
	}

	v2m.spis[spi - v2m.first_spi] = entry->gsi;

	return 0;
}

/*
 * Find SPI bound to the given MSI and return the associated GSI.
 */
static int gicv2m_translate_gsi(struct kvm *kvm, u32 gsi)
{
	int i;

	for (i = 0; i < v2m.num_spis; i++) {
		if (v2m.spis[i] == (int)gsi)
			return i + v2m.first_spi - KVM_IRQ_OFFSET;
	}

	/* Not an MSI */
	return gsi;
}

static bool gicv2m_can_signal_msi(struct kvm *kvm)
{
	return true;
}

/*
 * Instead of setting up MSI routes, virtual devices can also trigger them
 * manually (like a direct write to MSI_SETSPI). In this case, trigger the SPI
 * directly.
 */
static int gicv2m_signal_msi(struct kvm *kvm, struct kvm_msi *msi)
{
	int spi = msi->data & GICV2M_SPI_MASK;

	if (spi < v2m.first_spi || spi >= v2m.first_spi + v2m.num_spis) {
		pr_err("invalid SPI number %d", spi);
		return -EINVAL;
	}

	kvm__irq_trigger(kvm, spi);
	return 0;
}

static struct msi_routing_ops gicv2m_routing = {
	.update_route	= gicv2m_update_routing,
	.translate_gsi	= gicv2m_translate_gsi,
	.can_signal_msi	= gicv2m_can_signal_msi,
	.signal_msi	= gicv2m_signal_msi,
};

static void gicv2m_mmio_callback(struct kvm_cpu *vcpu, u64 addr, u8 *data,
				  u32 len, u8 is_write, void *ptr)
{
	if (is_write)
		return;

	addr -= v2m.base;

	switch (addr) {
		case GICV2M_MSI_TYPER:
			*(u32 *)data = GICV2M_MSI_TYPER_VAL(v2m.first_spi,
							    v2m.num_spis);
			break;
		case GICV2M_MSI_IIDR:
			*(u32 *)data = 0x0;
			break;
	}
}

int gic__create_gicv2m_frame(struct kvm *kvm, u64 base)
{
	int i;
	int irq = irq__alloc_line();

	v2m = (struct gicv2m_chip) {
		.first_spi	= irq,	/* Includes GIC_SPI_IRQ_BASE */
		.num_spis	= 64,	/* arbitrary */
		.base		= base,
		.size		= KVM_VGIC_V2M_SIZE,
	};

	v2m.spis = calloc(v2m.num_spis, sizeof(int));
	if (!v2m.spis)
		return -ENOMEM;

	v2m.spis[0] = -1;
	for (i = 1; i < v2m.num_spis; i++) {
		irq__alloc_line();
		v2m.spis[i] = -1;
	}

	msi_routing_ops = &gicv2m_routing;

	return kvm__register_mmio(kvm, base, KVM_VGIC_V2M_SIZE, false,
				  gicv2m_mmio_callback, kvm);
}
