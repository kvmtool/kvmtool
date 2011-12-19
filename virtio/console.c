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
#include "kvm/virtio-trans.h"

#include <linux/virtio_console.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>

#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#define VIRTIO_CONSOLE_QUEUE_SIZE	128
#define VIRTIO_CONSOLE_NUM_QUEUES	2
#define VIRTIO_CONSOLE_RX_QUEUE		0
#define VIRTIO_CONSOLE_TX_QUEUE		1

struct con_dev {
	pthread_mutex_t			mutex;

	struct virtio_trans		vtrans;
	struct virt_queue		vqs[VIRTIO_CONSOLE_NUM_QUEUES];
	struct virtio_console_config	config;
	u32				features;

	struct thread_pool__job		jobs[VIRTIO_CONSOLE_NUM_QUEUES];
};

static struct con_dev cdev = {
	.mutex				= PTHREAD_MUTEX_INITIALIZER,

	.config = {
		.cols			= 80,
		.rows			= 24,
		.max_nr_ports		= 1,
	},
};

static int compat_id = -1;

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

	if (term_readable(CONSOLE_VIRTIO, 0) && virt_queue__available(vq)) {
		head = virt_queue__get_iov(vq, iov, &out, &in, kvm);
		len = term_getc_iov(CONSOLE_VIRTIO, iov, in, 0);
		virt_queue__set_used_elem(vq, head, len);
		cdev.vtrans.trans_ops->signal_vq(kvm, &cdev.vtrans, vq - cdev.vqs);
	}

	mutex_unlock(&cdev.mutex);
}

void virtio_console__inject_interrupt(struct kvm *kvm)
{
	thread_pool__do_job(&cdev.jobs[VIRTIO_CONSOLE_RX_QUEUE]);
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
		len = term_putc_iov(CONSOLE_VIRTIO, iov, out, 0);
		virt_queue__set_used_elem(vq, head, len);
	}

}

static void set_config(struct kvm *kvm, void *dev, u8 data, u32 offset)
{
	struct con_dev *cdev = dev;

	((u8 *)(&cdev->config))[offset] = data;
}

static u8 get_config(struct kvm *kvm, void *dev, u32 offset)
{
	struct con_dev *cdev = dev;

	return ((u8 *)(&cdev->config))[offset];
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return 0;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	/* Unused */
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 pfn)
{
	struct virt_queue *queue;
	void *p;

	BUG_ON(vq >= VIRTIO_CONSOLE_NUM_QUEUES);

	compat__remove_message(compat_id);

	queue			= &cdev.vqs[vq];
	queue->pfn		= pfn;
	p			= guest_pfn_to_host(kvm, queue->pfn);

	vring_init(&queue->vring, VIRTIO_CONSOLE_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

	if (vq == VIRTIO_CONSOLE_TX_QUEUE)
		thread_pool__init_job(&cdev.jobs[vq], kvm, virtio_console_handle_callback, queue);
	else if (vq == VIRTIO_CONSOLE_RX_QUEUE)
		thread_pool__init_job(&cdev.jobs[vq], kvm, virtio_console__inject_interrupt_callback, queue);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct con_dev *cdev = dev;

	thread_pool__do_job(&cdev->jobs[vq]);

	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct con_dev *cdev = dev;

	return cdev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_CONSOLE_QUEUE_SIZE;
}

static struct virtio_ops con_dev_virtio_ops = (struct virtio_ops) {
	.set_config		= set_config,
	.get_config		= get_config,
	.get_host_features	= get_host_features,
	.set_guest_features	= set_guest_features,
	.init_vq		= init_vq,
	.notify_vq		= notify_vq,
	.get_pfn_vq		= get_pfn_vq,
	.get_size_vq		= get_size_vq,
};

void virtio_console__init(struct kvm *kvm)
{
	virtio_trans_init(&cdev.vtrans, VIRTIO_PCI);

	cdev.vtrans.trans_ops->init(kvm, &cdev.vtrans, &cdev, PCI_DEVICE_ID_VIRTIO_CONSOLE,
					VIRTIO_ID_CONSOLE, PCI_CLASS_CONSOLE);
	cdev.vtrans.virtio_ops = &con_dev_virtio_ops;

	if (compat_id != -1)
		compat_id = compat__add_message("virtio-console device was not detected",
						"While you have requested a virtio-console device, "
						"the guest kernel did not initialize it.\n"
						"Please make sure that the guest kernel was "
						"compiled with CONFIG_VIRTIO_CONSOLE=y enabled "
						"in its .config");
}
