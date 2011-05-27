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
static int	epoll_fd;
static LIST_HEAD(used_ioevents);

void ioeventfd__init(void)
{
	epoll_fd = epoll_create(IOEVENTFD_MAX_EVENTS);
	if (epoll_fd < 0)
		die("Failed creating epoll fd");
}

void ioeventfd__add_event(struct ioevent *ioevent)
{
	struct kvm_ioeventfd kvm_ioevent;
	struct epoll_event epoll_event;
	struct ioevent *new_ioevent;
	int event;

	new_ioevent = malloc(sizeof(*new_ioevent));
	if (new_ioevent == NULL)
		die("Failed allocating memory for new ioevent");

	*new_ioevent = *ioevent;
	event = new_ioevent->fd;

	kvm_ioevent = (struct kvm_ioeventfd) {
		.addr			= ioevent->io_addr,
		.len			= ioevent->io_len,
		.datamatch		= ioevent->datamatch,
		.fd			= event,
		.flags			= KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DATAMATCH,
	};

	if (ioctl(ioevent->fn_kvm->vm_fd, KVM_IOEVENTFD, &kvm_ioevent) != 0)
		die("Failed creating new ioeventfd");

	epoll_event = (struct epoll_event) {
		.events			= EPOLLIN,
		.data.ptr		= new_ioevent,
	};

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event, &epoll_event) != 0)
		die("Failed assigning new event to the epoll fd");

	list_add_tail(&new_ioevent->list, &used_ioevents);
}

void ioeventfd__del_event(u64 addr, u64 datamatch)
{
	struct kvm_ioeventfd kvm_ioevent;
	struct ioevent *ioevent;
	u8 found = 0;

	list_for_each_entry(ioevent, &used_ioevents, list) {
		if (ioevent->io_addr == addr) {
			found = 1;
			break;
		}
	}

	if (found == 0 || ioevent == NULL)
		return;

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
}

static void *ioeventfd__thread(void *param)
{
	for (;;) {
		int nfds, i;

		nfds = epoll_wait(epoll_fd, events, IOEVENTFD_MAX_EVENTS, -1);
		for (i = 0; i < nfds; i++) {
			u64 tmp;
			struct ioevent *ioevent;

			ioevent = events[i].data.ptr;

			if (read(ioevent->fd, &tmp, sizeof(tmp)) < 0)
				die("Failed reading event");

			ioevent->fn(ioevent->fn_kvm, ioevent->fn_ptr);
		}
	}

	return NULL;
}

void ioeventfd__start(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, ioeventfd__thread, NULL) != 0)
		die("Failed starting ioeventfd thread");
}
