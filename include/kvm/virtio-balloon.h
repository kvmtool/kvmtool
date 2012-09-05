#ifndef KVM__BLN_VIRTIO_H
#define KVM__BLN_VIRTIO_H

struct kvm;

int virtio_bln__init(struct kvm *kvm);
int virtio_bln__exit(struct kvm *kvm);

#endif /* KVM__BLN_VIRTIO_H */
