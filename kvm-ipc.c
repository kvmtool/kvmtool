#include "kvm/kvm-ipc.h"
#include "kvm/rwsem.h"
#include "kvm/read-write.h"
#include "kvm/util.h"

#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

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

int kvm_ipc__handle(int fd, struct kvm_ipc_msg *msg)
{
	void (*cb)(int fd, u32 type, u32 len, u8 *msg);

	if (msg->type >= KVM_IPC_MAX_MSGS)
		return -ENOSPC;

	down_read(&msgs_rwlock);
	cb = msgs[msg->type];
	up_read(&msgs_rwlock);

	if (cb == NULL) {
		pr_warning("No device handles type %u\n", msg->type);
		return -ENODEV;
	}

	cb(fd, msg->type, msg->len, msg->data);

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

static void kvm_ipc__new_data(int fd)
{
	struct kvm_ipc_msg *msg;
	u32 n;

	msg = malloc(sizeof(*msg));
	if (msg == NULL)
		goto done;

	n = read(fd, msg, sizeof(*msg));
	if (n != sizeof(*msg))
		goto done;

	msg = realloc(msg, sizeof(*msg) + msg->len);
	if (msg == NULL)
		goto done;

	n = read_in_full(fd, msg->data, msg->len);
	if (n != msg->len)
		goto done;

	kvm_ipc__handle(fd, msg);

done:
	free(msg);
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
				int client;

				client = kvm_ipc__new_conn(fd);
				kvm_ipc__new_data(client);
			} else if (event.events && (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
				kvm_ipc__close_conn(fd);
			} else {
				kvm_ipc__new_data(fd);
			}
		}
	}

	return NULL;
}

int kvm_ipc__start(int sock)
{
	struct epoll_event ev;

	server_fd = sock;

	epoll_fd = epoll_create(KVM_IPC_MAX_MSGS);

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = sock;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0)
		die("Failed starting IPC thread");

	stop_fd = eventfd(0, 0);
	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = stop_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, stop_fd, &ev) < 0)
		die("Failed adding stop event to epoll");

	if (pthread_create(&thread, NULL, kvm_ipc__thread, NULL) != 0)
		die("Failed starting IPC thread");

	return 0;
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
