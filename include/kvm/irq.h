#ifndef KVM__IRQ_H
#define KVM__IRQ_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>

#include "kvm/msi.h"

struct kvm;

int irq__alloc_line(void);

int irq__init(struct kvm *kvm);
int irq__exit(struct kvm *kvm);
int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg);

#endif
