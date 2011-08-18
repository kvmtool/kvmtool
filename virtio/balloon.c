#include "kvm/virtio-balloon.h"

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
#include <linux/virtio_balloon.h>

#include <linux/list.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#define NUM_VIRT_QUEUES		3
#define VIRTIO_BLN_QUEUE_SIZE	128
#define VIRTIO_BLN_INFLATE	0
#define VIRTIO_BLN_DEFLATE	1
#define VIRTIO_BLN_STATS	2

struct bln_dev {
	struct pci_device_header pci_hdr;
	struct list_head	list;

	u16			base_addr;
	u8			status;
	u8			isr;
	u16			config_vector;
	u32			host_features;

	/* virtio queue */
	u16			queue_selector;
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct thread_pool__job	jobs[NUM_VIRT_QUEUES];

	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];
	struct virtio_balloon_stat *cur_stat;
	u32			cur_stat_head;
	u16			stat_count;
	int			stat_waitfd;

	int			compat_id;
	struct virtio_balloon_config config;
};

static struct bln_dev bdev;
extern struct kvm *kvm;

static bool virtio_bln_dev_in(void *data, unsigned long offset, int size)
{
	u8 *config_space = (u8 *) &bdev.config;

	if (size != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_MSI_CONFIG_VECTOR]);

	return true;
}

static bool virtio_bln_dev_out(void *data, unsigned long offset, int size)
{
	u8 *config_space = (u8 *) &bdev.config;

	if (size != 1)
		return false;

	config_space[offset - VIRTIO_MSI_CONFIG_VECTOR] = *(u8 *)data;

	return true;
}

static bool virtio_bln_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset;
	bool ret = true;

	offset = port - bdev.base_addr;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, bdev.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret		= false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, bdev.vqs[bdev.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_BLN_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, bdev.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, bdev.isr);
		kvm__irq_line(kvm, bdev.pci_hdr.irq_line, VIRTIO_IRQ_LOW);
		bdev.isr = VIRTIO_IRQ_LOW;
		break;
	default:
		ret = virtio_bln_dev_in(data, offset, size);
		break;
	};

	return ret;
}

static bool virtio_bln_do_io_request(struct kvm *kvm, struct bln_dev *bdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLN_QUEUE_SIZE];
	unsigned int len = 0;
	u16 out, in, head;
	u32 *ptrs, i;

	head		= virt_queue__get_iov(queue, iov, &out, &in, kvm);
	ptrs		= iov[0].iov_base;
	len		= iov[0].iov_len / sizeof(u32);

	for (i = 0 ; i < len ; i++) {
		void *guest_ptr;

		guest_ptr = guest_flat_to_host(kvm, ptrs[i] << VIRTIO_BALLOON_PFN_SHIFT);
		if (queue == &bdev->vqs[VIRTIO_BLN_INFLATE]) {
			madvise(guest_ptr, 1 << VIRTIO_BALLOON_PFN_SHIFT, MADV_DONTNEED);
			bdev->config.actual++;
		} else if (queue == &bdev->vqs[VIRTIO_BLN_DEFLATE]) {
			bdev->config.actual--;
		}
	}

	virt_queue__set_used_elem(queue, head, len);

	return true;
}

static bool virtio_bln_do_stat_request(struct kvm *kvm, struct bln_dev *bdev, struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLN_QUEUE_SIZE];
	u16 out, in, head;
	struct virtio_balloon_stat *stat;
	u64 wait_val = 1;

	head = virt_queue__get_iov(queue, iov, &out, &in, kvm);
	stat = iov[0].iov_base;

	/* Initial empty stat buffer */
	if (bdev->cur_stat == NULL) {
		bdev->cur_stat = stat;
		bdev->cur_stat_head = head;

		return true;
	}

	memcpy(bdev->stats, stat, iov[0].iov_len);

	bdev->stat_count = iov[0].iov_len / sizeof(struct virtio_balloon_stat);
	bdev->cur_stat = stat;
	bdev->cur_stat_head = head;

	if (write(bdev->stat_waitfd, &wait_val, sizeof(wait_val)) <= 0)
		return -EFAULT;

	return 1;
}

static void virtio_bln_do_io(struct kvm *kvm, void *param)
{
	struct virt_queue *vq = param;

	if (vq == &bdev.vqs[VIRTIO_BLN_STATS]) {
		virtio_bln_do_stat_request(kvm, &bdev, vq);
		virt_queue__trigger_irq(vq, bdev.pci_hdr.irq_line, &bdev.isr, kvm);
		return;
	}

	while (virt_queue__available(vq)) {
		virtio_bln_do_io_request(kvm, &bdev, vq);
		virt_queue__trigger_irq(vq, bdev.pci_hdr.irq_line, &bdev.isr, kvm);
	}
}

static void ioevent_callback(struct kvm *kvm, void *param)
{
	thread_pool__do_job(param);
}

static bool virtio_bln_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	unsigned long offset;
	bool ret = true;
	struct ioevent ioevent;

	offset = port - bdev.base_addr;

	switch (offset) {
	case VIRTIO_MSI_QUEUE_VECTOR:
	case VIRTIO_PCI_GUEST_FEATURES:
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		compat__remove_message(bdev.compat_id);

		queue			= &bdev.vqs[bdev.queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		vring_init(&queue->vring, VIRTIO_BLN_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

		thread_pool__init_job(&bdev.jobs[bdev.queue_selector], kvm, virtio_bln_do_io, queue);

		ioevent = (struct ioevent) {
			.io_addr		= bdev.base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
			.io_len			= sizeof(u16),
			.fn			= ioevent_callback,
			.fn_ptr			= &bdev.jobs[bdev.queue_selector],
			.datamatch		= bdev.queue_selector,
			.fn_kvm			= kvm,
			.fd			= eventfd(0, 0),
		};

		ioeventfd__add_event(&ioevent);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		bdev.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;
		queue_index		= ioport__read16(data);
		thread_pool__do_job(&bdev.jobs[queue_index]);
		break;
	}
	case VIRTIO_PCI_STATUS:
		bdev.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		bdev.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	default:
		ret = virtio_bln_dev_out(data, offset, size);
		break;
	};

	return ret;
}

static struct ioport_operations virtio_bln_io_ops = {
	.io_in				= virtio_bln_pci_io_in,
	.io_out				= virtio_bln_pci_io_out,
};

static int virtio_bln__collect_stats(void)
{
	u64 tmp;

	virt_queue__set_used_elem(&bdev.vqs[VIRTIO_BLN_STATS], bdev.cur_stat_head,
				  sizeof(struct virtio_balloon_stat));
	virt_queue__trigger_irq(&bdev.vqs[VIRTIO_BLN_STATS], bdev.pci_hdr.irq_line,
				&bdev.isr, kvm);

	if (read(bdev.stat_waitfd, &tmp, sizeof(tmp)) <= 0)
		return -EFAULT;

	return 0;
}

static int virtio_bln__print_stats(void)
{
	u16 i;

	if (virtio_bln__collect_stats() < 0)
		return -EFAULT;

	printf("\n\n\t*** Guest memory statistics ***\n\n");
	for (i = 0; i < bdev.stat_count; i++) {
		switch (bdev.stats[i].tag) {
		case VIRTIO_BALLOON_S_SWAP_IN:
			printf("The amount of memory that has been swapped in (in bytes):");
			break;
		case VIRTIO_BALLOON_S_SWAP_OUT:
			printf("The amount of memory that has been swapped out to disk (in bytes):");
			break;
		case VIRTIO_BALLOON_S_MAJFLT:
			printf("The number of major page faults that have occurred:");
			break;
		case VIRTIO_BALLOON_S_MINFLT:
			printf("The number of minor page faults that have occurred:");
			break;
		case VIRTIO_BALLOON_S_MEMFREE:
			printf("The amount of memory not being used for any purpose (in bytes):");
			break;
		case VIRTIO_BALLOON_S_MEMTOT:
			printf("The total amount of memory available (in bytes):");
			break;
		}
		printf("%llu\n", bdev.stats[i].val);
	}
	printf("\n");

	return 0;
}

static void handle_sigmem(int sig)
{
	if (sig == SIGKVMADDMEM) {
		bdev.config.num_pages += 256;
	} else if (sig == SIGKVMDELMEM) {
		if (bdev.config.num_pages < 256)
			return;

		bdev.config.num_pages -= 256;
	} else if (sig == SIGKVMMEMSTAT) {
		virtio_bln__print_stats();

		return;
	}

	/* Notify that the configuration space has changed */
	bdev.isr = VIRTIO_PCI_ISR_CONFIG;
	kvm__irq_line(kvm, bdev.pci_hdr.irq_line, 1);
}

void virtio_bln__init(struct kvm *kvm)
{
	u8 pin, line, dev;
	u16 bdev_base_addr;

	signal(SIGKVMADDMEM, handle_sigmem);
	signal(SIGKVMDELMEM, handle_sigmem);
	signal(SIGKVMMEMSTAT, handle_sigmem);

	bdev_base_addr = ioport__register(IOPORT_EMPTY, &virtio_bln_io_ops, IOPORT_SIZE, &bdev);

	bdev.pci_hdr = (struct pci_device_header) {
		.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
		.device_id		= PCI_DEVICE_ID_VIRTIO_BLN,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= 0,
		.class			= 0x010000,
		.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
		.subsys_id		= VIRTIO_ID_BALLOON,
		.bar[0]			= bdev_base_addr | PCI_BASE_ADDRESS_SPACE_IO,
	};

	bdev.base_addr = bdev_base_addr;

	if (irq__register_device(VIRTIO_ID_BALLOON, &dev, &pin, &line) < 0)
		return;

	bdev.pci_hdr.irq_pin	= pin;
	bdev.pci_hdr.irq_line	= line;
	bdev.host_features	= 1 << VIRTIO_BALLOON_F_STATS_VQ;
	bdev.stat_waitfd	= eventfd(0, 0);
	memset(&bdev.config, 0, sizeof(struct virtio_balloon_config));

	pci__register(&bdev.pci_hdr, dev);

	bdev.compat_id = compat__add_message("virtio-balloon device was not detected",
						"While you have requested a virtio-balloon device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_VIRTIO_BALLOON.");
}
