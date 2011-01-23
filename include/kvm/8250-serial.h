#ifndef KVM__8250_SERIAL_H
#define KVM__8250_SERIAL_H

struct kvm;

void serial8250__init(struct kvm *kvm);
void serial8250__interrupt(struct kvm *kvm);

#endif /* KVM__8250_SERIAL_H */
