#ifndef KVM__PCI_H
#define KVM__PCI_H

#include <inttypes.h>

#include "pci_regs.h"

/*
 * PCI Configuration Mechanism #1 I/O ports. See Section 3.7.4.1.
 * ("Configuration Mechanism #1") of the PCI Local Bus Specification 2.1 for
 * details.
 */
#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc

struct pci_config_address {
	unsigned		zeros		: 2;		/* 1  .. 0  */
	unsigned		register_number	: 6;		/* 7  .. 2  */
	unsigned		function_number	: 3;		/* 10 .. 8  */
	unsigned		device_number	: 5;		/* 15 .. 11 */
	unsigned		bus_number	: 8;		/* 23 .. 16 */
	unsigned		reserved	: 7;		/* 30 .. 24 */
	unsigned		enable_bit	: 1;		/* 31       */
};

struct pci_device_header {
	uint16_t	vendor_id;
	uint16_t	device_id;
	uint16_t	command;
	uint16_t	status;
	uint16_t	revision_id		:  8;
	uint32_t	class			: 24;
	uint8_t		cacheline_size;
	uint8_t		latency_timer;
	uint8_t		header_type;
	uint8_t		BIST;
	uint32_t	BAR[6];
	uint32_t	card_bus;
	uint16_t	subsys_vendor_id;
	uint16_t	subsys_id;
	uint32_t	exp_rom_bar;
	uint32_t	capabilities		:  8;
	uint32_t	reserved1		: 24;
	uint32_t	reserved2;
	uint8_t		irq_line;
	uint8_t		irq_pin;
	uint8_t		min_gnt;
	uint8_t		max_lat;
};

void pci__init(void);

#endif /* KVM__PCI_H */
