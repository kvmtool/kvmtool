#ifndef ARM_COMMON__PCI_H
#define ARM_COMMON__PCI_H

struct kvm;
void pci__generate_fdt_nodes(void *fdt, struct kvm *kvm);

#endif /* ARM_COMMON__PCI_H */
