#ifndef KVM__EARLY_PRINTK_H
#define KVM__EARLY_PRINTK_H

struct kvm;

void early_printk__init(void);
void serial8250__interrupt(struct kvm *self);

#endif /* KVM__EARLY_PRINTK_H */
