#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>

#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/types.h>

#include "kvm/ioeventfd.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#define IOEVENTFD_MAX_EVENTS	20

static struct	epoll_event events[IOEVENTFD_MAX_EVENTS];
static int	epoll_fd, epoll_stop_fd;
static LIST_HEAD(used_ioevents);
static bool	ioeventfd_avail;

static void *ioeventfd__thread(void *param)
{
	u64 tmp = 1;

	for (;;) {
		int nfds, i;

		nfds = epoll_wait(epoll_fd, events, IOEVENTFD_MAX_EVENTS, -1);
		for (i = 0; i < nfds; i++) {
			struct ioevent *ioevent;

			if (events[i].data.fd == epoll_stop_fd)
				goto done;

			ioevent = events[i].data.ptr;

			if (read(ioevent->fd, &tmp, sizeof(tmp)) < 0)
				die("Failed reading event");

			ioevent->fn(ioevent->fn_kvm, ioevent->fn_ptr);
		}
	}

done:
	tmp = write(epoll_stop_fd, &tmp, sizeof(tmp));

	return NULL;
}

static int ioeventfd__start(void)
{
	pthread_t thread;

	if (!ioeventfd_avail)
		return -ENOSYS;

	return pthread_create(&thread, NULL, ioeventfd__thread, NULL);
}

int ioeventfd__init(struct kvm *kvm)
{
	struct epoll_event epoll_event = {.events = EPOLLIN};
	int r;

	ioeventfd_avail = kvm__supports_extension(kvm, KVM_CAP_IOEVENTFD);
	if (!ioeventfd_avail)
		return 1; /* Not fatal, but let caller determine no-go. */

	epoll_fd = epoll_create(IOEVENTFD_MAX_EVENTS);
	if (epoll_fd < 0)
		return -errno;

	epoll_stop_fd = eventfd(0, 0);
	epoll_event.data.fd = epoll_stop_fd;

	r = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, epoll_stop_fd, &epoll_event);
	if (r < 0)
		goto cleanup;

	r = ioeventfd__start();
	if (r < 0)
		goto cleanup;

	r = 0;

	return r;

cleanup:
	close(epoll_stop_fd);
	close(epoll_fd);

	return r;
}
base_init(ioeventfd__init);

int ioeventfd__exit(struct kvm *kvm)
{
	u64 tmp = 1;
	int r;

	if (!ioeventfd_avail)
		return 0;

	r = write(epoll_stop_fd, &tmp, sizeof(tmp));
	if (r < 0)
		return r;

	r = read(epoll_stop_fd, &tmp, sizeof(tmp));
	if (r < 0)
		return r;

	close(epoll_fd);
	close(epoll_stop_fd);

	return 0;
}
base_exit(ioeventfd__exit);

int ioeventfd__add_event(struct ioevent *ioevent, bool is_pio, bool poll_in_userspace)
{
	struct kvm_ioeventfd kvm_ioevent;
	struct epoll_event epoll_event;
	struct ioevent *new_ioevent;
	int event, r;

	if (!ioeventfd_avail)
		return -ENOSYS;

	new_ioevent = malloc(sizeof(*new_ioevent));
	if (new_ioevent == NULL)
		return -ENOMEM;

	*new_ioevent = *ioevent;
	event = new_ioevent->fd;

	kvm_ioevent = (struct kvm_ioeventfd) {
		.addr		= ioevent->io_addr,
		.len		= ioevent->io_len,
		.datamatch	= ioevent->datamatch,
		.fd		= event,
		.flags		= KVM_IOEVENTFD_FLAG_DATAMATCH,
	};

	if (is_pio)
		kvm_ioevent.flags |= KVM_IOEVENTFD_FLAG_PIO;

	r = ioctl(ioevent->fn_kvm->vm_fd, KVM_IOEVENTFD, &kvm_ioevent);
	if (r) {
		r = -errno;
		goto cleanup;
	}

	if (!poll_in_userspace)
		return 0;

	epoll_event = (struct epoll_event) {
		.events		= EPOLLIN,
		.data.ptr	= new_ioevent,
	};

	r = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event, &epoll_event);
	if (r) {
		r = -errno;
		goto cleanup;
	}

	list_add_tail(&new_ioevent->list, &used_ioevents);

	return 0;

cleanup:
	free(new_ioevent);
	return r;
}

int ioeventfd__del_event(u64 addr, u64 datamatch)
{
	struct kvm_ioeventfd kvm_ioevent;
	struct ioevent *ioevent;
	u8 found = 0;

	if (!ioeventfd_avail)
		return -ENOSYS;

	list_for_each_entry(ioevent, &used_ioevents, list) {
		if (ioevent->io_addr == addr) {
			found = 1;
			break;
		}
	}

	if (found == 0 || ioevent == NULL)
		return -ENOENT;

	kvm_ioevent = (struct kvm_ioeventfd) {
		.addr			= ioevent->io_addr,
		.len			= ioevent->io_len,
		.datamatch		= ioevent->datamatch,
		.flags			= KVM_IOEVENTFD_FLAG_PIO
					| KVM_IOEVENTFD_FLAG_DEASSIGN
					| KVM_IOEVENTFD_FLAG_DATAMATCH,
	};

	ioctl(ioevent->fn_kvm->vm_fd, KVM_IOEVENTFD, &kvm_ioevent);

	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ioevent->fd, NULL);

	list_del(&ioevent->list);

	close(ioevent->fd);
	free(ioevent);

	return 0;
}
