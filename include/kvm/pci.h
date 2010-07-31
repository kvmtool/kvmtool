#ifndef KVM__PCI_H
#define KVM__PCI_H

/*
 * PCI Configuration Mechanism #1 I/O ports. See Section 3.7.4.1.
 * ("Configuration Mechanism #1") of the PCI Local Bus Specification 2.1 for
 * details.
 */
#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc

#define PCI_NO_DEVICE		0xffffffff

struct pci_config_address {
	unsigned		zeros		: 2;		/* 1  .. 0  */
	unsigned		register_number	: 6;		/* 7  .. 2  */
	unsigned		function_number	: 3;		/* 10 .. 8  */
	unsigned		device_number	: 5;		/* 15 .. 11 */
	unsigned		bus_number	: 8;		/* 23 .. 16 */
	unsigned		reserved	: 7;		/* 30 .. 24 */
	unsigned		enable_bit	: 1;		/* 31       */
};

void pci__init(void);

#endif /* KVM__PCI_H */
