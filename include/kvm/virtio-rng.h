#ifndef KVM__RNG_VIRTIO_H
#define KVM__RNG_VIRTIO_H

struct kvm;

void virtio_rng__init(struct kvm *kvm);
void virtio_rng__delete_all(struct kvm *kvm);

#endif /* KVM__RNG_VIRTIO_H */
