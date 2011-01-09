#ifndef KVM__8250_SERIAL_H
#define KVM__8250_SERIAL_H

struct kvm;

void serial8250__init(void);
void serial8250__interrupt(struct kvm *self);

#endif /* KVM__8250_SERIAL_H */
