#ifndef KVM__IRQ_H
#define KVM__IRQ_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>

#include "kvm/msi.h"

struct kvm;

struct msi_routing_ops {
	int (*update_route)(struct kvm *kvm, struct kvm_irq_routing_entry *);
	bool (*can_signal_msi)(struct kvm *kvm);
	int (*signal_msi)(struct kvm *kvm, struct kvm_msi *msi);
	int (*translate_gsi)(struct kvm *kvm, u32 gsi);
};

extern struct msi_routing_ops *msi_routing_ops;
extern struct kvm_irq_routing *irq_routing;
extern int next_gsi;

int irq__alloc_line(void);
int irq__get_nr_allocated_lines(void);

int irq__init(struct kvm *kvm);
int irq__exit(struct kvm *kvm);

int irq__allocate_routing_entry(void);
int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg, u32 device_id);
void irq__update_msix_route(struct kvm *kvm, u32 gsi, struct msi_msg *msg);

bool irq__can_signal_msi(struct kvm *kvm);
int irq__signal_msi(struct kvm *kvm, struct kvm_msi *msi);

#endif
