#include "kvm/virtio-console.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/mutex.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/irq.h"
#include "kvm/guest_compat.h"

#include <linux/virtio_console.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>

#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#define VIRTIO_CONSOLE_QUEUE_SIZE	128
#define VIRTIO_CONSOLE_NUM_QUEUES	2
#define VIRTIO_CONSOLE_RX_QUEUE		0
#define VIRTIO_CONSOLE_TX_QUEUE		1

static struct pci_device_header virtio_console_pci_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_CONSOLE,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x078000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= VIRTIO_ID_CONSOLE,
};

struct con_dev {
	pthread_mutex_t			mutex;

	struct virt_queue		vqs[VIRTIO_CONSOLE_NUM_QUEUES];
	struct virtio_console_config	console_config;
	u32				host_features;
	u32				guest_features;
	u16				config_vector;
	u8				status;
	u8				isr;
	u16				queue_selector;
	u16				base_addr;
	int				compat_id;

	struct thread_pool__job		jobs[VIRTIO_CONSOLE_NUM_QUEUES];
};

static struct con_dev cdev = {
	.mutex				= PTHREAD_MUTEX_INITIALIZER,

	.console_config = {
		.cols			= 80,
		.rows			= 24,
		.max_nr_ports		= 1,
	},

	.host_features			= 0,
};

/*
 * Interrupts are injected for hvc0 only.
 */
static void virtio_console__inject_interrupt_callback(struct kvm *kvm, void *param)
{
	struct iovec iov[VIRTIO_CONSOLE_QUEUE_SIZE];
	struct virt_queue *vq;
	u16 out, in;
	u16 head;
	int len;

	mutex_lock(&cdev.mutex);

	vq = param;

	if (term_readable(CONSOLE_VIRTIO) && virt_queue__available(vq)) {
		head = virt_queue__get_iov(vq, iov, &out, &in, kvm);
		len = term_getc_iov(CONSOLE_VIRTIO, iov, in);
		virt_queue__set_used_elem(vq, head, len);
		virt_queue__trigger_irq(vq, virtio_console_pci_device.irq_line, &cdev.isr, kvm);
	}

	mutex_unlock(&cdev.mutex);
}

void virtio_console__inject_interrupt(struct kvm *kvm)
{
	thread_pool__do_job(&cdev.jobs[VIRTIO_CONSOLE_RX_QUEUE]);
}

static bool virtio_console_pci_io_device_specific_in(void *data, unsigned long offset, int size)
{
	u8 *config_space = (u8 *) &cdev.console_config;

	if (size != 1)
		return false;

	if ((offset - VIRTIO_MSI_CONFIG_VECTOR) > sizeof(struct virtio_console_config))
		pr_error("config offset is too big: %li", offset - VIRTIO_MSI_CONFIG_VECTOR);

	ioport__write8(data, config_space[offset - VIRTIO_MSI_CONFIG_VECTOR]);

	return true;
}

static bool virtio_console_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset = port - cdev.base_addr;
	bool ret = true;

	mutex_lock(&cdev.mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, cdev.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret = false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, cdev.vqs[cdev.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_CONSOLE_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret = false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, cdev.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, cdev.isr);
		kvm__irq_line(kvm, virtio_console_pci_device.irq_line, VIRTIO_IRQ_LOW);
		cdev.isr = VIRTIO_IRQ_LOW;
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, cdev.config_vector);
		break;
	default:
		ret = virtio_console_pci_io_device_specific_in(data, offset, size);
	};

	mutex_unlock(&cdev.mutex);

	return ret;
}

static void virtio_console_handle_callback(struct kvm *kvm, void *param)
{
	struct iovec iov[VIRTIO_CONSOLE_QUEUE_SIZE];
	struct virt_queue *vq;
	u16 out, in;
	u16 head;
	u32 len;

	vq = param;

	/*
	 * The current Linux implementation polls for the buffer
	 * to be used, rather than waiting for an interrupt.
	 * So there is no need to inject an interrupt for the tx path.
	 */

	while (virt_queue__available(vq)) {
		head = virt_queue__get_iov(vq, iov, &out, &in, kvm);
		len = term_putc_iov(CONSOLE_VIRTIO, iov, out);
		virt_queue__set_used_elem(vq, head, len);
	}

}

static bool virtio_console_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset = port - cdev.base_addr;
	bool ret = true;

	mutex_lock(&cdev.mutex);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		cdev.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		assert(cdev.queue_selector < VIRTIO_CONSOLE_NUM_QUEUES);

		compat__remove_message(cdev.compat_id);

		queue			= &cdev.vqs[cdev.queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		vring_init(&queue->vring, VIRTIO_CONSOLE_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

		if (cdev.queue_selector == VIRTIO_CONSOLE_TX_QUEUE)
			thread_pool__init_job(&cdev.jobs[cdev.queue_selector], kvm, virtio_console_handle_callback, queue);
		else if (cdev.queue_selector == VIRTIO_CONSOLE_RX_QUEUE)
			thread_pool__init_job(&cdev.jobs[cdev.queue_selector], kvm, virtio_console__inject_interrupt_callback, queue);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		cdev.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index		= ioport__read16(data);
		thread_pool__do_job(&cdev.jobs[queue_index]);
		break;
	}
	case VIRTIO_PCI_STATUS:
		cdev.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		cdev.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret			= false;
		break;
	};

	mutex_unlock(&cdev.mutex);

	return ret;
}

static struct ioport_operations virtio_console_io_ops = {
	.io_in			= virtio_console_pci_io_in,
	.io_out			= virtio_console_pci_io_out,
};

void virtio_console__init(struct kvm *kvm)
{
	u8 dev, line, pin;
	u16 console_base_addr;

	if (irq__register_device(VIRTIO_ID_CONSOLE, &dev, &pin, &line) < 0)
		return;

	virtio_console_pci_device.irq_pin	= pin;
	virtio_console_pci_device.irq_line	= line;
	console_base_addr			= ioport__register(IOPORT_EMPTY, &virtio_console_io_ops, IOPORT_SIZE, NULL);
	virtio_console_pci_device.bar[0]	= console_base_addr | PCI_BASE_ADDRESS_SPACE_IO;
	cdev.base_addr				= console_base_addr;
	pci__register(&virtio_console_pci_device, dev);

	cdev.compat_id = compat__add_message("virtio-console device was not detected",
						"While you have requested a virtio-console device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_VIRTIO_CONSOLE.");
}
