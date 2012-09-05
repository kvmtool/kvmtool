#ifndef KVM__VIRTIO_NET_H
#define KVM__VIRTIO_NET_H

#include "kvm/parse-options.h"

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

int virtio_net__init(struct kvm *kvm);
int virtio_net__exit(struct kvm *kvm);
int netdev_parser(const struct option *opt, const char *arg, int unset);

enum {
	NET_MODE_USER,
	NET_MODE_TAP
};

#endif /* KVM__VIRTIO_NET_H */
