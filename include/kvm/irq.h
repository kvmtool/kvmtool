#ifndef KVM__IRQ_H
#define KVM__IRQ_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>

#include "kvm/msi.h"

struct kvm;

extern struct kvm_irq_routing *irq_routing;
extern int next_gsi;

int irq__alloc_line(void);
int irq__get_nr_allocated_lines(void);

int irq__init(struct kvm *kvm);
int irq__exit(struct kvm *kvm);

int irq__allocate_routing_entry(void);
int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg, u32 device_id);
void irq__update_msix_route(struct kvm *kvm, u32 gsi, struct msi_msg *msg);

#endif
