#ifndef KVM__OF_PCI_H
#define KVM__OF_PCI_H

#include <linux/types.h>

/*
 * Definitions for implementing parts of the OpenFirmware PCI Bus Binding
 * Specification (IEEE Std 1275-1994).
 */

struct of_pci_unit_address {
	u32 hi, mid, lo;
} __attribute__((packed));

struct of_pci_irq_mask {
	struct of_pci_unit_address	pci_addr;
	u32				pci_pin;
} __attribute__((packed));

struct of_pci_ranges_entry {
	struct of_pci_unit_address	pci_addr;
	u64				cpu_addr;
	u64				length;
} __attribute__((packed));

/* Macros to operate with address in OF binding to PCI */
#define __b_x(x, p, l)		(((x) & ((1<<(l))-1)) << (p))
#define of_pci_b_n(x)		__b_x((x), 31, 1)	/* 0 if relocatable */
#define of_pci_b_p(x)		__b_x((x), 30, 1)	/* 1 if prefetchable */
#define of_pci_b_t(x)		__b_x((x), 29, 1)	/* 1 if the address is aliased */
#define of_pci_b_ss(x)		__b_x((x), 24, 2)	/* the space code */
#define of_pci_b_bbbbbbbb(x)	__b_x((x), 16, 8)	/* bus number */
#define of_pci_b_ddddd(x)	__b_x((x), 11, 5)	/* device number */
#define of_pci_b_fff(x)		__b_x((x), 8, 3)	/* function number */
#define of_pci_b_rrrrrrrr(x)	__b_x((x), 0, 8)	/* register number */

#define OF_PCI_SS_CONFIG	0
#define OF_PCI_SS_IO		1
#define OF_PCI_SS_M32		2
#define OF_PCI_SS_M64		3

#define OF_PCI_IRQ_MAP_MAX	256	/* 5 bit device + 3 bit pin */

#endif /* KVM__OF_PCI_H */
