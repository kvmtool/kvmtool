#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

struct kvm;

struct virtio_net_parameters {
	struct kvm *self;
	const char *host_ip;
};

void virtio_net__init(const struct virtio_net_parameters *params);

#endif /* KVM__VIRTIO_NET_H */
