#ifndef KVM__PCI_H
#define KVM__PCI_H

/* some known offsets and register names */
#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc

void pci__init(void);

#endif /* KVM__PCI_H */
