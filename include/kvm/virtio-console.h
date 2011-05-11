#ifndef KVM__CONSOLE_VIRTIO_H
#define KVM__CONSOLE_VIRTIO_H

struct kvm;

void virtio_console__init(struct kvm *kvm);
void virtio_console__inject_interrupt(struct kvm *kvm);

#endif /* KVM__CONSOLE_VIRTIO_H */
