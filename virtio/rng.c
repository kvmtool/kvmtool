#include "kvm/virtio-rng.h"

#include "kvm/virtio-pci-dev.h"

#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/irq.h"
#include "kvm/ioeventfd.h"
#include "kvm/guest_compat.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_rng.h>

#include <linux/list.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#define NUM_VIRT_QUEUES		1
#define VIRTIO_RNG_QUEUE_SIZE	128

struct rng_dev_job {
	struct virt_queue	*vq;
	struct rng_dev		*rdev;
	struct thread_pool__job	job_id;
};

struct rng_dev {
	struct pci_device_header pci_hdr;
	struct list_head	list;

	u16			base_addr;
	u8			status;
	u8			isr;
	u16			config_vector;
	int			fd;
	u32			vq_vector[NUM_VIRT_QUEUES];
	u32			msix_io_block;
	int			compat_id;

	/* virtio queue */
	u16			queue_selector;
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct rng_dev_job	jobs[NUM_VIRT_QUEUES];
};

static LIST_HEAD(rdevs);

static bool virtio_rng_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset;
	bool ret = true;
	struct rng_dev *rdev;

	rdev = ioport->priv;
	offset = port - rdev->base_addr;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
	case VIRTIO_PCI_GUEST_FEATURES:
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret		= false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, rdev->vqs[rdev->queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_RNG_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, rdev->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, rdev->isr);
		kvm__irq_line(kvm, rdev->pci_hdr.irq_line, VIRTIO_IRQ_LOW);
		rdev->isr = VIRTIO_IRQ_LOW;
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, rdev->config_vector);
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		ioport__write16(data, rdev->vq_vector[rdev->queue_selector]);
		break;
	default:
		ret		= false;
		break;
	};

	return ret;
}

static bool virtio_rng_do_io_request(struct kvm *kvm, struct rng_dev *rdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_RNG_QUEUE_SIZE];
	unsigned int len = 0;
	u16 out, in, head;

	head		= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	len		= readv(rdev->fd, iov, in);

	virt_queue__set_used_elem(queue, head, len);

	return true;
}

static void virtio_rng_do_io(struct kvm *kvm, void *param)
{
	struct rng_dev_job *job = param;
	struct virt_queue *vq = job->vq;
	struct rng_dev *rdev = job->rdev;

	while (virt_queue__available(vq))
		virtio_rng_do_io_request(kvm, rdev, vq);

	kvm__irq_line(kvm, rdev->pci_hdr.irq_line, VIRTIO_IRQ_HIGH);
}

static bool virtio_rng_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset;
	bool ret = true;
	struct rng_dev *rdev;

	rdev = ioport->priv;
	offset = port - rdev->base_addr;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		struct rng_dev_job *job;
		void *p;

		compat__remove_message(rdev->compat_id);

		queue			= &rdev->vqs[rdev->queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		job = &rdev->jobs[rdev->queue_selector];

		vring_init(&queue->vring, VIRTIO_RNG_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

		*job			= (struct rng_dev_job) {
			.vq			= queue,
			.rdev			= rdev,
		};

		thread_pool__init_job(&job->job_id, kvm, virtio_rng_do_io, job);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		rdev->queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;
		queue_index		= ioport__read16(data);
		thread_pool__do_job(&rdev->jobs[queue_index].job_id);
		break;
	}
	case VIRTIO_PCI_STATUS:
		rdev->status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		rdev->config_vector	= ioport__read16(data);
		break;
	case VIRTIO_MSI_QUEUE_VECTOR: {
		u32 gsi;
		u32 vec;

		vec = rdev->vq_vector[rdev->queue_selector] = ioport__read16(data);

		gsi = irq__add_msix_route(kvm,
					  rdev->pci_hdr.msix.table[vec].low,
					  rdev->pci_hdr.msix.table[vec].high,
					  rdev->pci_hdr.msix.table[vec].data);
		rdev->pci_hdr.irq_line = gsi;
		break;
	}
	default:
		ret			= false;
		break;
	};

	return ret;
}

static struct ioport_operations virtio_rng_io_ops = {
	.io_in				= virtio_rng_pci_io_in,
	.io_out				= virtio_rng_pci_io_out,
};

static void ioevent_callback(struct kvm *kvm, void *param)
{
	struct rng_dev_job *job = param;

	thread_pool__do_job(&job->job_id);
}

static void callback_mmio(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr)
{
	struct rng_dev *rdev = ptr;
	void *table = &rdev->pci_hdr.msix.table;
	if (is_write)
		memcpy(table + addr - rdev->msix_io_block, data, len);
	else
		memcpy(data, table + addr - rdev->msix_io_block, len);
}

void virtio_rng__init(struct kvm *kvm)
{
	u8 pin, line, dev, i;
	u16 rdev_base_addr;
	struct rng_dev *rdev;
	struct ioevent ioevent;

	rdev = malloc(sizeof(*rdev));
	if (rdev == NULL)
		return;

	rdev->msix_io_block = pci_get_io_space_block();

	rdev_base_addr = ioport__register(IOPORT_EMPTY, &virtio_rng_io_ops, IOPORT_SIZE, rdev);
	kvm__register_mmio(kvm, rdev->msix_io_block, 0x100, callback_mmio, rdev);

	rdev->pci_hdr = (struct pci_device_header) {
		.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
		.device_id		= PCI_DEVICE_ID_VIRTIO_RNG,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= 0,
		.class			= 0x010000,
		.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
		.subsys_id		= VIRTIO_ID_RNG,
		.bar[0]			= rdev_base_addr | PCI_BASE_ADDRESS_SPACE_IO,
		.bar[1]			= rdev->msix_io_block |
					PCI_BASE_ADDRESS_SPACE_MEMORY |
					PCI_BASE_ADDRESS_MEM_TYPE_64,
		/* bar[2] is the continuation of bar[1] for 64bit addressing */
		.bar[2]			= 0,
		.status			= PCI_STATUS_CAP_LIST,
		.capabilities		= (void *)&rdev->pci_hdr.msix - (void *)&rdev->pci_hdr,
	};

	rdev->pci_hdr.msix.cap = PCI_CAP_ID_MSIX;
	rdev->pci_hdr.msix.next = 0;
	rdev->pci_hdr.msix.table_size = (NUM_VIRT_QUEUES + 1) | PCI_MSIX_FLAGS_ENABLE;
	rdev->pci_hdr.msix.table_offset = 1; /* Use BAR 1 */

	rdev->config_vector = 0;
	rdev->base_addr = rdev_base_addr;
	rdev->fd = open("/dev/urandom", O_RDONLY);
	if (rdev->fd < 0)
		die("Failed initializing RNG");

	if (irq__register_device(VIRTIO_ID_RNG, &dev, &pin, &line) < 0)
		return;

	rdev->pci_hdr.irq_pin	= pin;
	rdev->pci_hdr.irq_line	= line;
	pci__register(&rdev->pci_hdr, dev);

	list_add_tail(&rdev->list, &rdevs);

	for (i = 0; i < NUM_VIRT_QUEUES; i++) {
		ioevent = (struct ioevent) {
			.io_addr		= rdev_base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
			.io_len			= sizeof(u16),
			.fn			= ioevent_callback,
			.fn_ptr			= &rdev->jobs[i],
			.datamatch		= i,
			.fn_kvm			= kvm,
			.fd			= eventfd(0, 0),
		};

		ioeventfd__add_event(&ioevent);
	}

	rdev->compat_id = compat__add_message("virtio-rng device was not detected",
						"While you have requested a virtio-rng device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_HW_RANDOM_VIRTIO.");
}

void virtio_rng__delete_all(struct kvm *kvm)
{
	while (!list_empty(&rdevs)) {
		struct rng_dev *rdev;

		rdev = list_first_entry(&rdevs, struct rng_dev, list);
		list_del(&rdev->list);
		ioeventfd__del_event(rdev->base_addr + VIRTIO_PCI_QUEUE_NOTIFY, 0);
		free(rdev);
	}
}
