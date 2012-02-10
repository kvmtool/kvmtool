#include "kvm/kvm-ipc.h"
#include "kvm/rwsem.h"
#include "kvm/read-write.h"
#include "kvm/util.h"

#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

struct kvm_ipc_head {
	u32 type;
	u32 len;
};

#define KVM_IPC_MAX_MSGS 16

static void (*msgs[KVM_IPC_MAX_MSGS])(int fd, u32 type, u32 len, u8 *msg);
static DECLARE_RWSEM(msgs_rwlock);
static int epoll_fd, server_fd, stop_fd;
static pthread_t thread;

int kvm_ipc__register_handler(u32 type, void (*cb)(int fd, u32 type, u32 len, u8 *msg))
{
	if (type >= KVM_IPC_MAX_MSGS)
		return -ENOSPC;

	down_write(&msgs_rwlock);
	msgs[type] = cb;
	up_write(&msgs_rwlock);

	return 0;
}

int kvm_ipc__send(int fd, u32 type)
{
	struct kvm_ipc_head head = {.type = type, .len = 0,};

	if (write_in_full(fd, &head, sizeof(head)) < 0)
		return -1;

	return 0;
}

int kvm_ipc__send_msg(int fd, u32 type, u32 len, u8 *msg)
{
	struct kvm_ipc_head head = {.type = type, .len = len,};

	if (write_in_full(fd, &head, sizeof(head)) < 0)
		return -1;

	if (write_in_full(fd, msg, len) < 0)
		return -1;

	return 0;
}

static int kvm_ipc__handle(int fd, u32 type, u32 len, u8 *data)
{
	void (*cb)(int fd, u32 type, u32 len, u8 *msg);

	if (type >= KVM_IPC_MAX_MSGS)
		return -ENOSPC;

	down_read(&msgs_rwlock);
	cb = msgs[type];
	up_read(&msgs_rwlock);

	if (cb == NULL) {
		pr_warning("No device handles type %u\n", type);
		return -ENODEV;
	}

	cb(fd, type, len, data);

	return 0;
}

static int kvm_ipc__new_conn(int fd)
{
	int client;
	struct epoll_event ev;

	client = accept(fd, NULL, NULL);
	if (client < 0)
		return -1;

	ev.events = EPOLLIN | EPOLLRDHUP;
	ev.data.fd = client;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev) < 0) {
		close(client);
		return -1;
	}

	return client;
}

static void kvm_ipc__close_conn(int fd)
{
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
}

static int kvm_ipc__receive(int fd)
{
	struct kvm_ipc_head head;
	u8 *msg = NULL;
	u32 n;

	n = read(fd, &head, sizeof(head));
	if (n != sizeof(head))
		goto done;

	msg = malloc(head.len);
	if (msg == NULL)
		goto done;

	n = read_in_full(fd, msg, head.len);
	if (n != head.len)
		goto done;

	kvm_ipc__handle(fd, head.type, head.len, msg);

	return 0;

done:
	free(msg);
	return -1;
}

static void *kvm_ipc__thread(void *param)
{
	struct epoll_event event;

	for (;;) {
		int nfds;

		nfds = epoll_wait(epoll_fd, &event, 1, -1);
		if (nfds > 0) {
			int fd = event.data.fd;

			if (fd == stop_fd && event.events & EPOLLIN) {
				break;
			} else if (fd == server_fd) {
				int client, r;

				client = kvm_ipc__new_conn(fd);
				/*
				 * Handle multiple IPC cmd at a time
				 */
				do {
					r = kvm_ipc__receive(client);
				} while	(r == 0);

			} else if (event.events && (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
				kvm_ipc__close_conn(fd);
			} else {
				kvm_ipc__receive(fd);
			}
		}
	}

	return NULL;
}

int kvm_ipc__start(int sock)
{
	int ret;
	struct epoll_event ev = {0};

	server_fd = sock;

	epoll_fd = epoll_create(KVM_IPC_MAX_MSGS);
	if (epoll_fd < 0) {
		ret = epoll_fd;
		goto err;
	}

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = sock;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0) {
		pr_err("Failed starting IPC thread");
		ret = -EFAULT;
		goto err_epoll;
	}

	stop_fd = eventfd(0, 0);
	if (stop_fd < 0) {
		ret = stop_fd;
		goto err_epoll;
	}

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = stop_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, stop_fd, &ev) < 0) {
		pr_err("Failed adding stop event to epoll");
		ret = -EFAULT;
		goto err_stop;
	}

	if (pthread_create(&thread, NULL, kvm_ipc__thread, NULL) != 0) {
		pr_err("Failed starting IPC thread");
		ret = -EFAULT;
		goto err_stop;
	}

	return 0;

err_stop:
	close(stop_fd);
err_epoll:
	close(epoll_fd);
err:
	return ret;
}

int kvm_ipc__stop(void)
{
	u64 val = 1;
	int ret;

	ret = write(stop_fd, &val, sizeof(val));
	if (ret < 0)
		return ret;

	close(server_fd);
	close(epoll_fd);

	return ret;
}
