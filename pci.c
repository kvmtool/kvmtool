#include "kvm/pci.h"

#include "kvm/ioport.h"

#include <stdint.h>

static struct pci_config_address pci_config_address;

static bool pci_config_address_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	struct pci_config_address *addr = data;

	pci_config_address	= *addr;

	return true;
}

static bool pci_config_address_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	struct pci_config_address *addr = data;

	*addr		=  pci_config_address;

	return true;
}

static struct ioport_operations pci_config_address_ops = {
	.io_in		= pci_config_address_in,
	.io_out		= pci_config_address_out,
};

static bool pci_config_data_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static bool pci_config_data_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static struct ioport_operations pci_config_data_ops = {
	.io_in		= pci_config_data_in,
	.io_out		= pci_config_data_out,
};

void pci__init(void)
{
	ioport__register(PCI_CONFIG_DATA,    &pci_config_data_ops);
	ioport__register(PCI_CONFIG_ADDRESS, &pci_config_address_ops);
}
