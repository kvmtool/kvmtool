#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

struct kvm;

struct virtio_net_parameters {
	struct kvm *kvm;
	const char *host_ip;
	char guest_mac[6];
	const char *script;
};

void virtio_net__init(const struct virtio_net_parameters *params);

#endif /* KVM__VIRTIO_NET_H */
