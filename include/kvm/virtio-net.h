#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

struct kvm;

struct virtio_net_parameters {
	struct kvm *kvm;
	const char *host_ip;
	char guest_mac[6];
	const char *script;
	int mode;
};

void virtio_net__init(const struct virtio_net_parameters *params);

#define NET_MODE_USER	0
#define NET_MODE_TAP	1

#endif /* KVM__VIRTIO_NET_H */
