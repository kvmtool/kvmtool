#include "kvm/virtio-console.h"
#include "kvm/virtio-pci.h"
#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/mutex.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#include <linux/virtio_console.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>

#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <termios.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#define VIRTIO_CONSOLE_IRQ		14
#define VIRTIO_CONSOLE_QUEUE_SIZE	128
#define VIRTIO_CONSOLE_NUM_QUEUES	2
#define VIRTIO_CONSOLE_RX_QUEUE		0
#define VIRTIO_CONSOLE_TX_QUEUE		1
#define PCI_VIRTIO_CONSOLE_DEVNUM	2

struct console_device {
	pthread_mutex_t			mutex;

	struct virt_queue		vqs[VIRTIO_CONSOLE_NUM_QUEUES];
	struct virtio_console_config	console_config;
	uint32_t			host_features;
	uint32_t			guest_features;
	uint16_t			config_vector;
	uint8_t				status;
	uint16_t			queue_selector;
};

static struct console_device console_device = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,

	.console_config = {
		.cols		= 80,
		.rows		= 24,
		.max_nr_ports	= 1,
	},

	.host_features		= 0,
};

/*
 * Interrupts are injected for hvc0 only.
 */
void virtio_console__inject_interrupt(struct kvm *self)
{
	struct iovec iov[VIRTIO_CONSOLE_QUEUE_SIZE];
	struct virt_queue *vq;
	uint16_t out, in;
	uint16_t head;
	int len;

	mutex_lock(&console_device.mutex);

	vq = &console_device.vqs[VIRTIO_CONSOLE_RX_QUEUE];

	if (term_readable(CONSOLE_VIRTIO) && virt_queue__available(vq)) {
		head = virt_queue__get_iov(vq, iov, &out, &in, self);
		len = term_getc_iov(CONSOLE_VIRTIO, iov, in);
		virt_queue__set_used_elem(vq, head, len);
		kvm__irq_line(self, VIRTIO_CONSOLE_IRQ, 1);
	}

	mutex_unlock(&console_device.mutex);
}

static bool virtio_console_pci_io_device_specific_in(void *data, unsigned long offset, int size, uint32_t count)
{
	uint8_t *config_space = (uint8_t *) &console_device.console_config;

	if (size != 1 || count != 1)
		return false;

	if ((offset - VIRTIO_PCI_CONFIG_NOMSI) > sizeof(struct virtio_console_config))
		error("config offset is too big: %li", offset - VIRTIO_PCI_CONFIG_NOMSI);

	ioport__write8(data, config_space[offset - VIRTIO_PCI_CONFIG_NOMSI]);

	return true;
}

static bool virtio_console_pci_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset = port - IOPORT_VIRTIO_CONSOLE;
	bool ret = true;

	mutex_lock(&console_device.mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, console_device.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret = false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, console_device.vqs[console_device.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_CONSOLE_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret = false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, console_device.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, 0x1);
		kvm__irq_line(self, VIRTIO_CONSOLE_IRQ, 0);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, console_device.config_vector);
		break;
	default:
		ret = virtio_console_pci_io_device_specific_in(data, offset, size, count);
	};

	mutex_unlock(&console_device.mutex);

	return ret;
}

static void virtio_console_handle_callback(struct kvm *self, uint16_t queue_index)
{
	struct iovec iov[VIRTIO_CONSOLE_QUEUE_SIZE];
	struct virt_queue *vq;
	uint16_t out, in;
	uint16_t head;
	uint32_t len;

	vq = &console_device.vqs[queue_index];

	if (queue_index == VIRTIO_CONSOLE_TX_QUEUE) {

		while (virt_queue__available(vq)) {
			head = virt_queue__get_iov(vq, iov, &out, &in, self);
			len = term_putc_iov(CONSOLE_VIRTIO, iov, out);
			virt_queue__set_used_elem(vq, head, len);
		}

		kvm__irq_line(self, VIRTIO_CONSOLE_IRQ, 1);
	}
}

static bool virtio_console_pci_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset = port - IOPORT_VIRTIO_CONSOLE;
	bool ret = true;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		console_device.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		assert(console_device.queue_selector < VIRTIO_CONSOLE_NUM_QUEUES);

		queue		= &console_device.vqs[console_device.queue_selector];
		queue->pfn	= ioport__read32(data);
		p		= guest_flat_to_host(self, queue->pfn << 12);

		vring_init(&queue->vring, VIRTIO_CONSOLE_QUEUE_SIZE, p, 4096);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		console_device.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		uint16_t queue_index;
		queue_index	= ioport__read16(data);
		virtio_console_handle_callback(self, queue_index);
		break;
	}
	case VIRTIO_PCI_STATUS:
		console_device.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		console_device.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret = false;
	};

	mutex_unlock(&console_device.mutex);
	return ret;
}

static struct ioport_operations virtio_console_io_ops = {
	.io_in	= virtio_console_pci_io_in,
	.io_out	= virtio_console_pci_io_out,
};

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_CONSOLE		0x1003
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_CONSOLE		0x0003

static struct pci_device_header virtio_console_pci_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_CONSOLE,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x078000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_CONSOLE,
	.bar[0]			= IOPORT_VIRTIO_CONSOLE | PCI_BASE_ADDRESS_SPACE_IO,
	.irq_pin		= 3,
	.irq_line		= VIRTIO_CONSOLE_IRQ,
};

void virtio_console__init(struct kvm *self)
{
	pci__register(&virtio_console_pci_device, PCI_VIRTIO_CONSOLE_DEVNUM);
	ioport__register(IOPORT_VIRTIO_CONSOLE, &virtio_console_io_ops, IOPORT_VIRTIO_CONSOLE_SIZE);
}
