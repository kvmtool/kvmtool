#ifndef KVM__VSOCK_VIRTIO_H
#define KVM__VSOCK_VIRTIO_H

struct kvm;

int virtio_vsock_init(struct kvm *kvm);
int virtio_vsock_exit(struct kvm *kvm);

#endif /* KVM__VSOCK_VIRTIO_H */
