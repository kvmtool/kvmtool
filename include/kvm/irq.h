#ifndef KVM__IRQ_H
#define KVM__IRQ_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>

#include "kvm/msi.h"

struct kvm;

struct irq_line {
	u8			line;
	struct list_head	node;
};

struct pci_dev {
	struct rb_node		node;
	u32			id;
	u8			pin;
	struct list_head	lines;
};

int irq__register_device(u32 dev, u8 *num, u8 *pin, u8 *line);

struct rb_node *irq__get_pci_tree(void);

int irq__init(struct kvm *kvm);
int irq__exit(struct kvm *kvm);
int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg);

#endif
