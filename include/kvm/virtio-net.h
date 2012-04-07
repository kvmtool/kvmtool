#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

struct kvm;

struct virtio_net_params {
	const char *guest_ip;
	const char *host_ip;
	const char *script;
	const char *trans;
	char guest_mac[6];
	char host_mac[6];
	struct kvm *kvm;
	int mode;
	int vhost;
	int fd;
};

void virtio_net__init(const struct virtio_net_params *params);

enum {
	NET_MODE_USER,
	NET_MODE_TAP
};

#endif /* KVM__VIRTIO_NET_H */
