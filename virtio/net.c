#include "kvm/virtio-pci-dev.h"
#include "kvm/virtio-net.h"
#include "kvm/virtio.h"
#include "kvm/types.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/irq.h"
#include "kvm/uip.h"
#include "kvm/ioeventfd.h"
#include "kvm/guest_compat.h"
#include "kvm/virtio-pci.h"

#include <linux/virtio_net.h>
#include <linux/if_tun.h>
#include <linux/types.h>

#include <arpa/inet.h>
#include <net/if.h>

#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define VIRTIO_NET_QUEUE_SIZE		128
#define VIRTIO_NET_NUM_QUEUES		2
#define VIRTIO_NET_RX_QUEUE		0
#define VIRTIO_NET_TX_QUEUE		1

struct net_dev;

extern struct kvm *kvm;

struct net_dev_operations {
	int (*rx)(struct iovec *iov, u16 in, struct net_dev *ndev);
	int (*tx)(struct iovec *iov, u16 in, struct net_dev *ndev);
};

struct net_dev {
	pthread_mutex_t			mutex;
	struct virtio_pci		vpci;

	struct virt_queue		vqs[VIRTIO_NET_NUM_QUEUES];
	struct virtio_net_config	config;
	u32				features;
	int				compat_id;

	pthread_t			io_rx_thread;
	pthread_mutex_t			io_rx_lock;
	pthread_cond_t			io_rx_cond;

	pthread_t			io_tx_thread;
	pthread_mutex_t			io_tx_lock;
	pthread_cond_t			io_tx_cond;

	int				tap_fd;
	char				tap_name[IFNAMSIZ];

	int				mode;

	struct uip_info			info;
	struct net_dev_operations	*ops;
};

static struct net_dev ndev = {
	.mutex	= PTHREAD_MUTEX_INITIALIZER,

	.config = {
		.status			= VIRTIO_NET_S_LINK_UP,
	},
	.info = {
		.buf_nr			= 20,
	}
};

static void *virtio_net_rx_thread(void *p)
{
	struct iovec iov[VIRTIO_NET_QUEUE_SIZE];
	struct virt_queue *vq;
	struct kvm *kvm;
	u16 out, in;
	u16 head;
	int len;

	kvm	= p;
	vq	= &ndev.vqs[VIRTIO_NET_RX_QUEUE];

	while (1) {

		mutex_lock(&ndev.io_rx_lock);
		if (!virt_queue__available(vq))
			pthread_cond_wait(&ndev.io_rx_cond, &ndev.io_rx_lock);
		mutex_unlock(&ndev.io_rx_lock);

		while (virt_queue__available(vq)) {

			head = virt_queue__get_iov(vq, iov, &out, &in, kvm);

			len = ndev.ops->rx(iov, in, &ndev);

			virt_queue__set_used_elem(vq, head, len);

			/* We should interrupt guest right now, otherwise latency is huge. */
			virtio_pci__signal_vq(kvm, &ndev.vpci, VIRTIO_NET_RX_QUEUE);
		}

	}

	pthread_exit(NULL);
	return NULL;

}

static void *virtio_net_tx_thread(void *p)
{
	struct iovec iov[VIRTIO_NET_QUEUE_SIZE];
	struct virt_queue *vq;
	struct kvm *kvm;
	u16 out, in;
	u16 head;
	int len;

	kvm	= p;
	vq	= &ndev.vqs[VIRTIO_NET_TX_QUEUE];

	while (1) {
		mutex_lock(&ndev.io_tx_lock);
		if (!virt_queue__available(vq))
			pthread_cond_wait(&ndev.io_tx_cond, &ndev.io_tx_lock);
		mutex_unlock(&ndev.io_tx_lock);

		while (virt_queue__available(vq)) {

			head = virt_queue__get_iov(vq, iov, &out, &in, kvm);

			len = ndev.ops->tx(iov, out, &ndev);

			virt_queue__set_used_elem(vq, head, len);
		}

		virtio_pci__signal_vq(kvm, &ndev.vpci, VIRTIO_NET_TX_QUEUE);
	}

	pthread_exit(NULL);

	return NULL;

}

static void virtio_net_handle_callback(struct kvm *kvm, u16 queue_index)
{
	switch (queue_index) {
	case VIRTIO_NET_TX_QUEUE:
		mutex_lock(&ndev.io_tx_lock);
		pthread_cond_signal(&ndev.io_tx_cond);
		mutex_unlock(&ndev.io_tx_lock);
		break;
	case VIRTIO_NET_RX_QUEUE:
		mutex_lock(&ndev.io_rx_lock);
		pthread_cond_signal(&ndev.io_rx_cond);
		mutex_unlock(&ndev.io_rx_lock);
		break;
	default:
		pr_warning("Unknown queue index %u", queue_index);
	}
}

static void ioevent_callback(struct kvm *kvm, void *param)
{
	virtio_net_handle_callback(kvm, (u64)(long)param);
}

static bool virtio_net__tap_init(const struct virtio_net_parameters *params)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	int pid, status, offload, hdr_len;
	struct sockaddr_in sin = {0};
	struct ifreq ifr;

	ndev.tap_fd = open("/dev/net/tun", O_RDWR);
	if (ndev.tap_fd < 0) {
		pr_warning("Unable to open /dev/net/tun");
		goto fail;
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;
	if (ioctl(ndev.tap_fd, TUNSETIFF, &ifr) < 0) {
		pr_warning("Config tap device error. Are you root?");
		goto fail;
	}

	strncpy(ndev.tap_name, ifr.ifr_name, sizeof(ndev.tap_name));

	if (ioctl(ndev.tap_fd, TUNSETNOCSUM, 1) < 0) {
		pr_warning("Config tap device TUNSETNOCSUM error");
		goto fail;
	}

	hdr_len = sizeof(struct virtio_net_hdr);
	if (ioctl(ndev.tap_fd, TUNSETVNETHDRSZ, &hdr_len) < 0) {
		pr_warning("Config tap device TUNSETVNETHDRSZ error");
	}

	offload = TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6 | TUN_F_UFO;
	if (ioctl(ndev.tap_fd, TUNSETOFFLOAD, offload) < 0) {
		pr_warning("Config tap device TUNSETOFFLOAD error");
		goto fail;
	}

	if (strcmp(params->script, "none")) {
		pid = fork();
		if (pid == 0) {
			execl(params->script, params->script, ndev.tap_name, NULL);
			_exit(1);
		} else {
			waitpid(pid, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
				pr_warning("Fail to setup tap by %s", params->script);
				goto fail;
			}
		}
	} else {
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, ndev.tap_name, sizeof(ndev.tap_name));
		sin.sin_addr.s_addr = inet_addr(params->host_ip);
		memcpy(&(ifr.ifr_addr), &sin, sizeof(ifr.ifr_addr));
		ifr.ifr_addr.sa_family = AF_INET;
		if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
			pr_warning("Could not set ip address on tap device");
			goto fail;
		}
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ndev.tap_name, sizeof(ndev.tap_name));
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
		pr_warning("Could not bring tap device up");

	close(sock);

	return 1;

fail:
	if (sock >= 0)
		close(sock);
	if (ndev.tap_fd >= 0)
		close(ndev.tap_fd);

	return 0;
}

static void virtio_net__io_thread_init(struct kvm *kvm)
{
	pthread_mutex_init(&ndev.io_rx_lock, NULL);
	pthread_cond_init(&ndev.io_tx_cond, NULL);

	pthread_mutex_init(&ndev.io_rx_lock, NULL);
	pthread_cond_init(&ndev.io_tx_cond, NULL);

	pthread_create(&ndev.io_rx_thread, NULL, virtio_net_rx_thread, (void *)kvm);
	pthread_create(&ndev.io_tx_thread, NULL, virtio_net_tx_thread, (void *)kvm);
}

static inline int tap_ops_tx(struct iovec *iov, u16 out, struct net_dev *ndev)
{
	return writev(ndev->tap_fd, iov, out);
}

static inline int tap_ops_rx(struct iovec *iov, u16 in, struct net_dev *ndev)
{
	return readv(ndev->tap_fd, iov, in);
}

static inline int uip_ops_tx(struct iovec *iov, u16 out, struct net_dev *ndev)
{
	return uip_tx(iov, out, &ndev->info);
}

static inline int uip_ops_rx(struct iovec *iov, u16 in, struct net_dev *ndev)
{
	return uip_rx(iov, in, &ndev->info);
}

static struct net_dev_operations tap_ops = {
	.rx	= tap_ops_rx,
	.tx	= tap_ops_tx,
};

static struct net_dev_operations uip_ops = {
	.rx	= uip_ops_rx,
	.tx	= uip_ops_tx,
};

static void set_config(struct kvm *kvm, void *dev, u8 data, u32 offset)
{
	struct net_dev *ndev = dev;

	((u8 *)(&ndev->config))[offset] = data;
}

static u8 get_config(struct kvm *kvm, void *dev, u32 offset)
{
	struct net_dev *ndev = dev;

	return ((u8 *)(&ndev->config))[offset];
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return 1UL << VIRTIO_NET_F_MAC
		| 1UL << VIRTIO_NET_F_CSUM
		| 1UL << VIRTIO_NET_F_HOST_UFO
		| 1UL << VIRTIO_NET_F_HOST_TSO4
		| 1UL << VIRTIO_NET_F_HOST_TSO6
		| 1UL << VIRTIO_NET_F_GUEST_UFO
		| 1UL << VIRTIO_NET_F_GUEST_TSO4
		| 1UL << VIRTIO_NET_F_GUEST_TSO6;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	struct net_dev *ndev = dev;

	ndev->features = features;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 pfn)
{
	struct net_dev *ndev = dev;
	struct virt_queue *queue;
	void *p;
	struct ioevent ioevent;

	compat__remove_message(ndev->compat_id);

	queue			= &ndev->vqs[vq];
	queue->pfn		= pfn;
	p			= guest_pfn_to_host(kvm, queue->pfn);

	vring_init(&queue->vring, VIRTIO_NET_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

	ioevent = (struct ioevent) {
		.io_addr	= ndev->vpci.base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
		.io_len		= sizeof(u16),
		.fn		= ioevent_callback,
		.fn_ptr		= (void *)(u64)vq,
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

	ioeventfd__add_event(&ioevent);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	virtio_net_handle_callback(kvm, vq);

	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct net_dev *ndev = dev;

	return ndev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_NET_QUEUE_SIZE;
}

void virtio_net__init(const struct virtio_net_parameters *params)
{
	int i;

	for (i = 0 ; i < 6 ; i++) {
		ndev.config.mac[i]		= params->guest_mac[i];
		ndev.info.guest_mac.addr[i]	= params->guest_mac[i];
		ndev.info.host_mac.addr[i]	= params->host_mac[i];
	}

	ndev.mode = params->mode;
	if (ndev.mode == NET_MODE_TAP) {
		virtio_net__tap_init(params);
		ndev.ops = &tap_ops;
	} else {
		ndev.info.host_ip		= ntohl(inet_addr(params->host_ip));
		ndev.info.guest_ip		= ntohl(inet_addr(params->guest_ip));
		ndev.info.guest_netmask		= ntohl(inet_addr("255.255.255.0"));
		uip_init(&ndev.info);
		ndev.ops = &uip_ops;
	}

	virtio_pci__init(kvm, &ndev.vpci, &ndev, PCI_DEVICE_ID_VIRTIO_NET, VIRTIO_ID_NET);
	ndev.vpci.ops = (struct virtio_pci_ops) {
		.set_config		= set_config,
		.get_config		= get_config,
		.get_host_features	= get_host_features,
		.set_guest_features	= set_guest_features,
		.init_vq		= init_vq,
		.notify_vq		= notify_vq,
		.get_pfn_vq		= get_pfn_vq,
		.get_size_vq		= get_size_vq,
	};

	virtio_net__io_thread_init(params->kvm);

	ndev.compat_id = compat__add_message("virtio-net device was not detected",
						"While you have requested a virtio-net device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_VIRTIO_NET.");
}
