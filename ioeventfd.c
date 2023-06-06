#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>

#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/types.h>

#include "kvm/epoll.h"
#include "kvm/ioeventfd.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#define IOEVENTFD_MAX_EVENTS	20

static LIST_HEAD(used_ioevents);
static bool	ioeventfd_avail;
static struct kvm__epoll epoll;

static void ioeventfd__handle_event(struct kvm *kvm, struct epoll_event *ev)
{
	u64 tmp;
	struct ioevent *ioevent = ev->data.ptr;

	if (read(ioevent->fd, &tmp, sizeof(tmp)) < 0)
		die("Failed reading event");

	ioevent->fn(ioevent->fn_kvm, ioevent->fn_ptr);
}

int ioeventfd__init(struct kvm *kvm)
{
	ioeventfd_avail = kvm__supports_extension(kvm, KVM_CAP_IOEVENTFD);
	if (!ioeventfd_avail)
		return 1; /* Not fatal, but let caller determine no-go. */

	return epoll__init(kvm, &epoll, "ioeventfd-worker",
			   ioeventfd__handle_event);
}
base_init(ioeventfd__init);

int ioeventfd__exit(struct kvm *kvm)
{
	if (!ioeventfd_avail)
		return 0;

	epoll__exit(&epoll);
	return 0;
}
base_exit(ioeventfd__exit);

int ioeventfd__add_event(struct ioevent *ioevent, int flags)
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

	/*
	 * For architectures that don't recognize PIO accesses, always register
	 * on the MMIO bus. Otherwise PIO accesses will cause returns to
	 * userspace.
	 */
	if (KVM_IOEVENTFD_HAS_PIO && flags & IOEVENTFD_FLAG_PIO)
		kvm_ioevent.flags |= KVM_IOEVENTFD_FLAG_PIO;

	r = ioctl(ioevent->fn_kvm->vm_fd, KVM_IOEVENTFD, &kvm_ioevent);
	if (r) {
		r = -errno;
		goto cleanup;
	}

	if (flags & IOEVENTFD_FLAG_USER_POLL) {
		epoll_event = (struct epoll_event) {
			.events		= EPOLLIN,
			.data.ptr	= new_ioevent,
		};

		r = epoll_ctl(epoll.fd, EPOLL_CTL_ADD, event, &epoll_event);
		if (r) {
			r = -errno;
			goto cleanup;
		}
	}

	new_ioevent->flags = kvm_ioevent.flags;
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
		if (ioevent->io_addr == addr &&
		    ioevent->datamatch == datamatch) {
			found = 1;
			break;
		}
	}

	if (found == 0 || ioevent == NULL)
		return -ENOENT;

	kvm_ioevent = (struct kvm_ioeventfd) {
		.fd			= ioevent->fd,
		.addr			= ioevent->io_addr,
		.len			= ioevent->io_len,
		.datamatch		= ioevent->datamatch,
		.flags			= ioevent->flags
					| KVM_IOEVENTFD_FLAG_DEASSIGN,
	};

	ioctl(ioevent->fn_kvm->vm_fd, KVM_IOEVENTFD, &kvm_ioevent);

	epoll_ctl(epoll.fd, EPOLL_CTL_DEL, ioevent->fd, NULL);

	list_del(&ioevent->list);

	close(ioevent->fd);
	free(ioevent);

	return 0;
}
