#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <dirent.h>

#include "kvm/kvm-ipc.h"
#include "kvm/rwsem.h"
#include "kvm/read-write.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/builtin-debug.h"
#include "kvm/strbuf.h"
#include "kvm/kvm-cpu.h"
#include "kvm/8250-serial.h"

struct kvm_ipc_head {
	u32 type;
	u32 len;
};

#define KVM_IPC_MAX_MSGS 16

#define KVM_SOCK_SUFFIX		".sock"
#define KVM_SOCK_SUFFIX_LEN	((ssize_t)sizeof(KVM_SOCK_SUFFIX) - 1)

extern __thread struct kvm_cpu *current_kvm_cpu;
static void (*msgs[KVM_IPC_MAX_MSGS])(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg);
static DECLARE_RWSEM(msgs_rwlock);
static int epoll_fd, server_fd, stop_fd;
static pthread_t thread;

static int kvm__create_socket(struct kvm *kvm)
{
	char full_name[PATH_MAX];
	unsigned int s;
	struct sockaddr_un local;
	int len, r;

	/* This usually 108 bytes long */
	BUILD_BUG_ON(sizeof(local.sun_path) < 32);

	snprintf(full_name, sizeof(full_name), "%s/%s%s",
		 kvm__get_dir(), kvm->cfg.guest_name, KVM_SOCK_SUFFIX);
	if (access(full_name, F_OK) == 0) {
		pr_err("Socket file %s already exist", full_name);
		return -EEXIST;
	}

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return s;
	local.sun_family = AF_UNIX;
	strlcpy(local.sun_path, full_name, sizeof(local.sun_path));
	len = strlen(local.sun_path) + sizeof(local.sun_family);
	r = bind(s, (struct sockaddr *)&local, len);
	if (r < 0)
		goto fail;

	r = listen(s, 5);
	if (r < 0)
		goto fail;

	return s;

fail:
	close(s);
	return r;
}

void kvm__remove_socket(const char *name)
{
	char full_name[PATH_MAX];

	snprintf(full_name, sizeof(full_name), "%s/%s%s",
		 kvm__get_dir(), name, KVM_SOCK_SUFFIX);
	unlink(full_name);
}

int kvm__get_sock_by_instance(const char *name)
{
	int s, len, r;
	char sock_file[PATH_MAX];
	struct sockaddr_un local;

	snprintf(sock_file, sizeof(sock_file), "%s/%s%s",
		 kvm__get_dir(), name, KVM_SOCK_SUFFIX);
	s = socket(AF_UNIX, SOCK_STREAM, 0);

	local.sun_family = AF_UNIX;
	strlcpy(local.sun_path, sock_file, sizeof(local.sun_path));
	len = strlen(local.sun_path) + sizeof(local.sun_family);

	r = connect(s, &local, len);
	if (r < 0 && errno == ECONNREFUSED) {
		/* Tell the user clean ghost socket file */
		pr_err("\"%s\" could be a ghost socket file, please remove it",
				sock_file);
		return r;
	} else if (r < 0) {
		return r;
	}

	return s;
}

int kvm__enumerate_instances(int (*callback)(const char *name, int fd))
{
	int sock;
	DIR *dir;
	struct dirent entry, *result;
	int ret = 0;

	dir = opendir(kvm__get_dir());
	if (!dir)
		return -errno;

	for (;;) {
		readdir_r(dir, &entry, &result);
		if (result == NULL)
			break;
		if (entry.d_type == DT_SOCK) {
			ssize_t name_len = strlen(entry.d_name);
			char *p;

			if (name_len <= KVM_SOCK_SUFFIX_LEN)
				continue;

			p = &entry.d_name[name_len - KVM_SOCK_SUFFIX_LEN];
			if (memcmp(KVM_SOCK_SUFFIX, p, KVM_SOCK_SUFFIX_LEN))
				continue;

			*p = 0;
			sock = kvm__get_sock_by_instance(entry.d_name);
			if (sock < 0)
				continue;
			ret = callback(entry.d_name, sock);
			close(sock);
			if (ret < 0)
				break;
		}
	}

	closedir(dir);

	return ret;
}

int kvm_ipc__register_handler(u32 type, void (*cb)(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg))
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

static int kvm_ipc__handle(struct kvm *kvm, int fd, u32 type, u32 len, u8 *data)
{
	void (*cb)(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg);

	if (type >= KVM_IPC_MAX_MSGS)
		return -ENOSPC;

	down_read(&msgs_rwlock);
	cb = msgs[type];
	up_read(&msgs_rwlock);

	if (cb == NULL) {
		pr_warning("No device handles type %u\n", type);
		return -ENODEV;
	}

	cb(kvm, fd, type, len, data);

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

static int kvm_ipc__receive(struct kvm *kvm, int fd)
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

	kvm_ipc__handle(kvm, fd, head.type, head.len, msg);

	return 0;

done:
	free(msg);
	return -1;
}

static void *kvm_ipc__thread(void *param)
{
	struct epoll_event event;
	struct kvm *kvm = param;

	kvm__set_thread_name("kvm-ipc");

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
					r = kvm_ipc__receive(kvm, client);
				} while	(r == 0);

			} else if (event.events & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
				kvm_ipc__close_conn(fd);
			} else {
				kvm_ipc__receive(kvm, fd);
			}
		}
	}

	return NULL;
}

static void kvm__pid(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	pid_t pid = getpid();
	int r = 0;

	if (type == KVM_IPC_PID)
		r = write(fd, &pid, sizeof(pid));

	if (r < 0)
		pr_warning("Failed sending PID");
}

static void handle_stop(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	if (WARN_ON(type != KVM_IPC_STOP || len))
		return;

	kvm_cpu__reboot(kvm);
}

/* Pause/resume the guest using SIGUSR2 */
static int is_paused;

static void handle_pause(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	if (WARN_ON(len))
		return;

	if (type == KVM_IPC_RESUME && is_paused) {
		kvm->vm_state = KVM_VMSTATE_RUNNING;
		kvm__continue(kvm);
	} else if (type == KVM_IPC_PAUSE && !is_paused) {
		kvm->vm_state = KVM_VMSTATE_PAUSED;
		ioctl(kvm->vm_fd, KVM_KVMCLOCK_CTRL);
		kvm__pause(kvm);
	} else {
		return;
	}

	is_paused = !is_paused;
}

static void handle_vmstate(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	int r = 0;

	if (type == KVM_IPC_VMSTATE)
		r = write(fd, &kvm->vm_state, sizeof(kvm->vm_state));

	if (r < 0)
		pr_warning("Failed sending VMSTATE");
}

/*
 * Serialize debug printout so that the output of multiple vcpus does not
 * get mixed up:
 */
static int printout_done;

static void handle_sigusr1(int sig)
{
	struct kvm_cpu *cpu = current_kvm_cpu;
	int fd = kvm_cpu__get_debug_fd();

	if (!cpu || cpu->needs_nmi)
		return;

	dprintf(fd, "\n #\n # vCPU #%ld's dump:\n #\n", cpu->cpu_id);
	kvm_cpu__show_registers(cpu);
	kvm_cpu__show_code(cpu);
	kvm_cpu__show_page_tables(cpu);
	fflush(stdout);
	printout_done = 1;
}

static void handle_debug(struct kvm *kvm, int fd, u32 type, u32 len, u8 *msg)
{
	int i;
	struct debug_cmd_params *params;
	u32 dbg_type;
	u32 vcpu;

	if (WARN_ON(type != KVM_IPC_DEBUG || len != sizeof(*params)))
		return;

	params = (void *)msg;
	dbg_type = params->dbg_type;
	vcpu = params->cpu;

	if (dbg_type & KVM_DEBUG_CMD_TYPE_SYSRQ)
		serial8250__inject_sysrq(kvm, params->sysrq);

	if (dbg_type & KVM_DEBUG_CMD_TYPE_NMI) {
		if ((int)vcpu >= kvm->nrcpus)
			return;

		kvm->cpus[vcpu]->needs_nmi = 1;
		pthread_kill(kvm->cpus[vcpu]->thread, SIGUSR1);
	}

	if (!(dbg_type & KVM_DEBUG_CMD_TYPE_DUMP))
		return;

	for (i = 0; i < kvm->nrcpus; i++) {
		struct kvm_cpu *cpu = kvm->cpus[i];

		if (!cpu)
			continue;

		printout_done = 0;

		kvm_cpu__set_debug_fd(fd);
		pthread_kill(cpu->thread, SIGUSR1);
		/*
		 * Wait for the vCPU to dump state before signalling
		 * the next thread. Since this is debug code it does
		 * not matter that we are burning CPU time a bit:
		 */
		while (!printout_done)
			sleep(0);
	}

	close(fd);

	serial8250__inject_sysrq(kvm, 'p');
}

int kvm_ipc__init(struct kvm *kvm)
{
	int ret;
	int sock = kvm__create_socket(kvm);
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

	if (pthread_create(&thread, NULL, kvm_ipc__thread, kvm) != 0) {
		pr_err("Failed starting IPC thread");
		ret = -EFAULT;
		goto err_stop;
	}

	kvm_ipc__register_handler(KVM_IPC_PID, kvm__pid);
	kvm_ipc__register_handler(KVM_IPC_DEBUG, handle_debug);
	kvm_ipc__register_handler(KVM_IPC_PAUSE, handle_pause);
	kvm_ipc__register_handler(KVM_IPC_RESUME, handle_pause);
	kvm_ipc__register_handler(KVM_IPC_STOP, handle_stop);
	kvm_ipc__register_handler(KVM_IPC_VMSTATE, handle_vmstate);
	signal(SIGUSR1, handle_sigusr1);

	return 0;

err_stop:
	close(stop_fd);
err_epoll:
	close(epoll_fd);
err:
	return ret;
}
base_init(kvm_ipc__init);

int kvm_ipc__exit(struct kvm *kvm)
{
	u64 val = 1;
	int ret;

	ret = write(stop_fd, &val, sizeof(val));
	if (ret < 0)
		return ret;

	close(server_fd);
	close(epoll_fd);

	kvm__remove_socket(kvm->cfg.guest_name);

	return ret;
}
base_exit(kvm_ipc__exit);
