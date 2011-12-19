#ifndef KVM__RNG_VIRTIO_H
#define KVM__RNG_VIRTIO_H

struct kvm;

int virtio_rng__init(struct kvm *kvm);
int virtio_rng__exit(struct kvm *kvm);

#endif /* KVM__RNG_VIRTIO_H */
