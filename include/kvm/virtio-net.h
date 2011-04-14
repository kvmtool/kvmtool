#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

struct kvm;
void virtio_net__init(struct kvm *self, const char *host_ip_addr);

#endif /* KVM__VIRTIO_NET_H */
