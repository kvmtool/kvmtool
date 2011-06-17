#ifndef KVM__VIRTIO_9P_H
#define KVM__VIRTIO_9P_H

struct kvm;

void virtio_9p__init(struct kvm *kvm, const char *root, const char *tag_name);

#endif
