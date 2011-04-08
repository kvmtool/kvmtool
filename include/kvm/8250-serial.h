#ifndef KVM__8250_SERIAL_H
#define KVM__8250_SERIAL_H

struct kvm;

void serial8250__init(struct kvm *kvm);
void serial8250__inject_interrupt(struct kvm *kvm);
void serial8250__inject_sysrq(struct kvm *kvm);

#endif /* KVM__8250_SERIAL_H */
