#include "kvm/virtio-net.h"
#include "kvm/virtio-pci.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/types.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#include <linux/virtio_net.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#define VIRTIO_NET_IRQ		14
#define VIRTIO_NET_QUEUE_SIZE	128
#define VIRTIO_NET_NUM_QUEUES	2
#define VIRTIO_NET_RX_QUEUE	0
#define VIRTIO_NET_TX_QUEUE	1
#define PCI_VIRTIO_NET_DEVNUM	3

struct net_device {
	pthread_mutex_t			mutex;

	struct virt_queue		vqs[VIRTIO_NET_NUM_QUEUES];
	struct virtio_net_config	net_config;
	uint32_t			host_features;
	uint32_t			guest_features;
	uint16_t			config_vector;
	uint8_t				status;
	uint16_t			queue_selector;

	pthread_t			io_rx_thread;
	pthread_mutex_t			io_rx_mutex;
	pthread_cond_t			io_rx_cond;

	pthread_t			io_tx_thread;
	pthread_mutex_t			io_tx_mutex;
	pthread_cond_t			io_tx_cond;

	int				tap_fd;
	char				tap_name[IFNAMSIZ];
};

static struct net_device net_device = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,

	.net_config = {
		.mac		= {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
		.status		= VIRTIO_NET_S_LINK_UP,
	},

	.host_features		= 1UL << VIRTIO_NET_F_MAC,
};

static void *virtio_net_rx_thread(void *p)
{
	struct iovec iov[VIRTIO_NET_QUEUE_SIZE];
	struct virt_queue *vq;
	struct kvm *self;
	uint16_t out, in;
	uint16_t head;
	int len;

	self = p;
	vq = &net_device.vqs[VIRTIO_NET_RX_QUEUE];

	while (1) {
		mutex_lock(&net_device.io_rx_mutex);
		if (!virt_queue__available(vq))
			pthread_cond_wait(&net_device.io_rx_cond, &net_device.io_rx_mutex);
		mutex_unlock(&net_device.io_rx_mutex);

		while (virt_queue__available(vq)) {
			head = virt_queue__get_iov(vq, iov, &out, &in, self);

			/* We do not specify GSO or CSUM features, So we can ignore virtio_net_hdr */
			len = readv(net_device.tap_fd, iov + 1, in - 1);

			/* However, We have to tell guest we have write the virtio_net_hdr */
			virt_queue__set_used_elem(vq, head, sizeof(struct virtio_net_hdr) + len);

			/* We should interrupt guest right now, otherwise latency is huge. */
			kvm__irq_line(self, VIRTIO_NET_IRQ, 1);
		}

	}

	pthread_exit(NULL);
	return NULL;

}

static void *virtio_net_tx_thread(void *p)
{
	struct iovec iov[VIRTIO_NET_QUEUE_SIZE];
	struct virt_queue *vq;
	struct kvm *self;
	uint16_t out, in;
	uint16_t head;
	int len;

	self = p;
	vq = &net_device.vqs[VIRTIO_NET_TX_QUEUE];

	while (1) {
		mutex_lock(&net_device.io_tx_mutex);
		if (!virt_queue__available(vq))
			pthread_cond_wait(&net_device.io_tx_cond, &net_device.io_tx_mutex);
		mutex_unlock(&net_device.io_tx_mutex);

		while (virt_queue__available(vq)) {
			head = virt_queue__get_iov(vq, iov, &out, &in, self);
			len = writev(net_device.tap_fd, iov + 1, out - 1);
			virt_queue__set_used_elem(vq, head, len);
		}

		kvm__irq_line(self, VIRTIO_NET_IRQ, 1);
	}

	pthread_exit(NULL);
	return NULL;

}
static bool virtio_net_pci_io_device_specific_in(void *data, unsigned long offset, int size, uint32_t count)
{
	uint8_t *config_space = (uint8_t *) &net_device.net_config;

	if (size != 1 || count != 1)
		return false;

	if ((offset - VIRTIO_PCI_CONFIG_NOMSI) > sizeof(struct virtio_net_config))
		error("config offset is too big: %li", offset - VIRTIO_PCI_CONFIG_NOMSI);

	ioport__write8(data, config_space[offset - VIRTIO_PCI_CONFIG_NOMSI]);

	return true;
}

static bool virtio_net_pci_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset = port - IOPORT_VIRTIO_NET;
	bool ret = true;

	mutex_lock(&net_device.mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, net_device.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret = false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, net_device.vqs[net_device.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_NET_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret = false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, net_device.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, 0x1);
		kvm__irq_line(self, VIRTIO_NET_IRQ, 0);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, net_device.config_vector);
		break;
	default:
		ret = virtio_net_pci_io_device_specific_in(data, offset, size, count);
	};

	mutex_unlock(&net_device.mutex);

	return ret;
}

static void virtio_net_handle_callback(struct kvm *self, uint16_t queue_index)
{
	if (queue_index == VIRTIO_NET_TX_QUEUE) {

		mutex_lock(&net_device.io_tx_mutex);
		pthread_cond_signal(&net_device.io_tx_cond);
		mutex_unlock(&net_device.io_tx_mutex);

	} else if (queue_index == VIRTIO_NET_RX_QUEUE) {

		mutex_lock(&net_device.io_rx_mutex);
		pthread_cond_signal(&net_device.io_rx_cond);
		mutex_unlock(&net_device.io_rx_mutex);

	}
}

static bool virtio_net_pci_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset = port - IOPORT_VIRTIO_NET;
	bool ret = true;

	mutex_lock(&net_device.mutex);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		net_device.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		assert(net_device.queue_selector < VIRTIO_NET_NUM_QUEUES);

		queue		= &net_device.vqs[net_device.queue_selector];
		queue->pfn	= ioport__read32(data);
		p		= guest_flat_to_host(self, queue->pfn << 12);

		vring_init(&queue->vring, VIRTIO_NET_QUEUE_SIZE, p, 4096);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		net_device.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		uint16_t queue_index;
		queue_index	= ioport__read16(data);
		virtio_net_handle_callback(self, queue_index);
		break;
	}
	case VIRTIO_PCI_STATUS:
		net_device.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		net_device.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret = false;
	};

	mutex_unlock(&net_device.mutex);
	return ret;
}

static struct ioport_operations virtio_net_io_ops = {
	.io_in	= virtio_net_pci_io_in,
	.io_out	= virtio_net_pci_io_out,
};

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_NET		0x1000
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_NET		0x0001

static struct pci_device_header virtio_net_pci_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_NET,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x020000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_NET,
	.bar[0]			= IOPORT_VIRTIO_NET | PCI_BASE_ADDRESS_SPACE_IO,
	.irq_pin		= 3,
	.irq_line		= VIRTIO_NET_IRQ,
};

static void virtio_net__tap_init(const char *host_ip_addr)
{
	struct ifreq ifr;
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sin = {0};

	net_device.tap_fd = open("/dev/net/tun", O_RDWR);
	if (net_device.tap_fd < 0)
		die("Unable to open /dev/net/tun\n");

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

	if (ioctl(net_device.tap_fd, TUNSETIFF, &ifr) < 0)
		die("Config tap device error. Are you root?");

	strncpy(net_device.tap_name, ifr.ifr_name, sizeof(net_device.tap_name));

	ioctl(net_device.tap_fd, TUNSETNOCSUM, 1);


	memset(&ifr, 0, sizeof(ifr));

	strncpy(ifr.ifr_name, net_device.tap_name, sizeof(net_device.tap_name));

	sin.sin_addr.s_addr = inet_addr(host_ip_addr);
	memcpy(&(ifr.ifr_addr), &sin, sizeof(ifr.ifr_addr));
	ifr.ifr_addr.sa_family = AF_INET;

	if (ioctl(sock, SIOCSIFADDR, &ifr) < 0)
		warning("Can not set ip address on tap device");

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, net_device.tap_name, sizeof(net_device.tap_name));
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
		warning("Could not bring tap device up");

	close(sock);
}

static void virtio_net__io_thread_init(struct kvm *self)
{
	pthread_mutex_init(&net_device.io_rx_mutex, NULL);
	pthread_cond_init(&net_device.io_tx_cond, NULL);

	pthread_mutex_init(&net_device.io_rx_mutex, NULL);
	pthread_cond_init(&net_device.io_tx_cond, NULL);

	pthread_create(&net_device.io_rx_thread, NULL, virtio_net_rx_thread, (void *)self);
	pthread_create(&net_device.io_tx_thread, NULL, virtio_net_tx_thread, (void *)self);
}

void virtio_net__init(struct kvm *self, const char *host_ip_addr)
{
	pci__register(&virtio_net_pci_device, PCI_VIRTIO_NET_DEVNUM);
	ioport__register(IOPORT_VIRTIO_NET, &virtio_net_io_ops, IOPORT_VIRTIO_NET_SIZE);

	virtio_net__tap_init(host_ip_addr);
	virtio_net__io_thread_init(self);
}
