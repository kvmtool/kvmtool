#include "kvm/pci.h"

#include "kvm/ioport.h"

#include <stdint.h>

static uint32_t pci_cse;	/* PCI configuration space enable */

static bool pci_cse_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint32_t *p = data;

	pci_cse		= *p;

	return true;
}

static bool pci_cse_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint32_t *p = data;

	*p		= pci_cse;

	return true;
}

static struct ioport_operations pci_cse_ops = {
	.io_in		= pci_cse_in,
	.io_out		= pci_cse_out,
};

static bool pci_mechanism_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static bool pci_mechanism_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static struct ioport_operations pci_mechanism_ops = {
	.io_in		= pci_mechanism_in,
	.io_out		= pci_mechanism_out,
};

void pci__init(void)
{
	ioport__register(0xcfb, &pci_mechanism_ops);
	ioport__register(0xcf8, &pci_cse_ops);
}
