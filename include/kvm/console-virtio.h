#ifndef KVM__CONSOLE_VIRTIO_H
#define KVM__CONSOLE_VIRTIO_H

struct kvm;

void virtio_console__init(struct kvm *self);
void virtio_console__inject_interrupt(struct kvm *self);

#endif /* KVM__CONSOLE_VIRTIO_H */
