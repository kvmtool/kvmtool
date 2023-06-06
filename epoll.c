#include <sys/eventfd.h>

#include "kvm/epoll.h"

#define EPOLLFD_MAX_EVENTS	20

static void *epoll__thread(void *param)
{
	u64 stop;
	int nfds, i;
	struct kvm__epoll *epoll = param;
	struct kvm *kvm = epoll->kvm;
	struct epoll_event events[EPOLLFD_MAX_EVENTS];

	kvm__set_thread_name(epoll->name);

	for (;;) {
		nfds = epoll_wait(epoll->fd, events, EPOLLFD_MAX_EVENTS, -1);
		for (i = 0; i < nfds; i++) {
			if (events[i].data.ptr == &epoll->stop_fd)
				goto done;

			epoll->handle_event(kvm, &events[i]);
		}
	}
done:
	if (read(epoll->stop_fd, &stop, sizeof(stop)) < 0)
		pr_warning("%s: read(stop) failed with %d", __func__, errno);
	if (write(epoll->stop_fd, &stop, sizeof(stop)) < 0)
		pr_warning("%s: write(stop) failed with %d", __func__, errno);
	return NULL;
}

int epoll__init(struct kvm *kvm, struct kvm__epoll *epoll,
		const char *name, epoll__event_handler_t handle_event)
{
	int r;
	struct epoll_event stop_event = {
		.events = EPOLLIN,
		.data.ptr = &epoll->stop_fd,
	};

	epoll->kvm = kvm;
	epoll->name = name;
	epoll->handle_event = handle_event;

	epoll->fd = epoll_create(EPOLLFD_MAX_EVENTS);
	if (epoll->fd < 0)
		return -errno;

	epoll->stop_fd = eventfd(0, 0);
	if (epoll->stop_fd < 0) {
		r = -errno;
		goto err_close_fd;
	}

	r = epoll_ctl(epoll->fd, EPOLL_CTL_ADD, epoll->stop_fd, &stop_event);
	if (r < 0)
		goto err_close_all;

	r = pthread_create(&epoll->thread, NULL, epoll__thread, epoll);
	if (r < 0)
		goto err_close_all;

	return 0;

err_close_all:
	close(epoll->stop_fd);
err_close_fd:
	close(epoll->fd);

	return r;
}

int epoll__exit(struct kvm__epoll *epoll)
{
	int r;
	u64 stop = 1;

	r = write(epoll->stop_fd, &stop, sizeof(stop));
	if (r < 0)
		return r;

	r = read(epoll->stop_fd, &stop, sizeof(stop));
	if (r < 0)
		return r;

	close(epoll->stop_fd);
	close(epoll->fd);
	return 0;
}
