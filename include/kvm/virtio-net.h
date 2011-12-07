#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

struct kvm;

struct virtio_net_params {
	const char *guest_ip;
	const char *host_ip;
	const char *script;
	char guest_mac[6];
	char host_mac[6];
	struct kvm *kvm;
	int mode;
	int vhost;
	int fd;
};

void virtio_net__init(const struct virtio_net_params *params);

#define NET_MODE_USER	0
#define NET_MODE_TAP	1

#endif /* KVM__VIRTIO_NET_H */
