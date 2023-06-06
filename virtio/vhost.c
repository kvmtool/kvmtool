#include "kvm/irq.h"
#include "kvm/virtio.h"
#include "kvm/epoll.h"

#include <linux/kvm.h>
#include <linux/vhost.h>
#include <linux/list.h>

#include <sys/eventfd.h>

static struct kvm__epoll epoll;

static void virtio_vhost_signal_vq(struct kvm *kvm, struct epoll_event *ev)
{
	int r;
	u64 tmp;
	struct virt_queue *queue = ev->data.ptr;

	if (read(queue->irqfd, &tmp, sizeof(tmp)) < 0)
		pr_warning("%s: failed to read eventfd", __func__);

	r = queue->vdev->ops->signal_vq(kvm, queue->vdev, queue->index);
	if (r)
		pr_warning("%s failed to signal virtqueue", __func__);
}

static int virtio_vhost_start_poll(struct kvm *kvm)
{
	if (epoll.fd)
		return 0;

	if (epoll__init(kvm, &epoll, "vhost-irq-worker",
			virtio_vhost_signal_vq))
		return -1;

	return 0;
}

static int virtio_vhost_stop_poll(struct kvm *kvm)
{
	if (epoll.fd)
		epoll__exit(&epoll);
	return 0;
}
base_exit(virtio_vhost_stop_poll);

void virtio_vhost_init(struct kvm *kvm, int vhost_fd)
{
	struct kvm_mem_bank *bank;
	struct vhost_memory *mem;
	int i = 0, r;

	r = virtio_vhost_start_poll(kvm);
	if (r)
		die("Unable to start vhost polling thread\n");

	mem = calloc(1, sizeof(*mem) +
		     kvm->mem_slots * sizeof(struct vhost_memory_region));
	if (mem == NULL)
		die("Failed allocating memory for vhost memory map");

	list_for_each_entry(bank, &kvm->mem_banks, list) {
		mem->regions[i] = (struct vhost_memory_region) {
			.guest_phys_addr = bank->guest_phys_addr,
			.memory_size	 = bank->size,
			.userspace_addr	 = (unsigned long)bank->host_addr,
		};
		i++;
	}
	mem->nregions = i;

	r = ioctl(vhost_fd, VHOST_SET_OWNER);
	if (r != 0)
		die_perror("VHOST_SET_OWNER failed");

	r = ioctl(vhost_fd, VHOST_SET_MEM_TABLE, mem);
	if (r != 0)
		die_perror("VHOST_SET_MEM_TABLE failed");

	free(mem);
}

static int virtio_vhost_get_irqfd(struct virt_queue *queue)
{
	if (!queue->irqfd) {
		queue->irqfd = eventfd(0, 0);
		if (queue->irqfd < 0)
			die_perror("eventfd()");
	}
	return queue->irqfd;
}

void virtio_vhost_set_vring(struct kvm *kvm, int vhost_fd, u32 index,
			    struct virt_queue *queue)
{
	int r;
	struct vhost_vring_addr addr = {
		.index = index,
		.desc_user_addr = (u64)(unsigned long)queue->vring.desc,
		.avail_user_addr = (u64)(unsigned long)queue->vring.avail,
		.used_user_addr = (u64)(unsigned long)queue->vring.used,
	};
	struct vhost_vring_state state = { .index = index };
	struct vhost_vring_file file = {
		.index	= index,
		.fd	= virtio_vhost_get_irqfd(queue),
	};
	struct epoll_event event = {
		.events = EPOLLIN,
		.data.ptr = queue,
	};

	queue->index = index;

	if (queue->endian != VIRTIO_ENDIAN_HOST)
		die("VHOST requires the same endianness in guest and host");

	state.num = queue->vring.num;
	r = ioctl(vhost_fd, VHOST_SET_VRING_NUM, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_NUM failed");

	state.num = 0;
	r = ioctl(vhost_fd, VHOST_SET_VRING_BASE, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_BASE failed");

	r = ioctl(vhost_fd, VHOST_SET_VRING_ADDR, &addr);
	if (r < 0)
		die_perror("VHOST_SET_VRING_ADDR failed");

	r = ioctl(vhost_fd, VHOST_SET_VRING_CALL, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_CALL failed");

	r = epoll_ctl(epoll.fd, EPOLL_CTL_ADD, file.fd, &event);
	if (r < 0)
		die_perror("EPOLL_CTL_ADD vhost call fd");
}

void virtio_vhost_set_vring_kick(struct kvm *kvm, int vhost_fd,
				 u32 index, int event_fd)
{
	int r;
	struct vhost_vring_file file = {
		.index	= index,
		.fd	= event_fd,
	};

	r = ioctl(vhost_fd, VHOST_SET_VRING_KICK, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_KICK failed");
}

void virtio_vhost_set_vring_irqfd(struct kvm *kvm, u32 gsi,
				  struct virt_queue *queue)
{
	int r;
	int fd = virtio_vhost_get_irqfd(queue);

	if (queue->gsi)
		irq__del_irqfd(kvm, queue->gsi, fd);
	else
		/* Disconnect user polling thread */
		epoll_ctl(epoll.fd, EPOLL_CTL_DEL, fd, NULL);

	/* Connect the direct IRQFD route */
	r = irq__add_irqfd(kvm, gsi, fd, -1);
	if (r < 0)
		die_perror("KVM_IRQFD failed");

	queue->gsi = gsi;
}

void virtio_vhost_reset_vring(struct kvm *kvm, int vhost_fd, u32 index,
			      struct virt_queue *queue)

{
	struct vhost_vring_file file = {
		.index	= index,
		.fd	= -1,
	};

	if (!queue->irqfd)
		return;

	if (queue->gsi) {
		irq__del_irqfd(kvm, queue->gsi, queue->irqfd);
		queue->gsi = 0;
	}

	epoll_ctl(epoll.fd, EPOLL_CTL_DEL, queue->irqfd, NULL);

	if (ioctl(vhost_fd, VHOST_SET_VRING_CALL, &file))
		perror("SET_VRING_CALL");
	close(queue->irqfd);
	queue->irqfd = 0;
}
