#ifndef KVM__8250_SERIAL_H
#define KVM__8250_SERIAL_H

struct kvm;

int serial8250__init(struct kvm *kvm);
int serial8250__exit(struct kvm *kvm);
void serial8250__update_consoles(struct kvm *kvm);
void serial8250__inject_sysrq(struct kvm *kvm, char sysrq);

#endif /* KVM__8250_SERIAL_H */
