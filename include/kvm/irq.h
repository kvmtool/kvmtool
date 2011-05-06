#ifndef KVM__IRQ_H
#define KVM__IRQ_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>

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

#endif
