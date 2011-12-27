#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-list.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

static bool run;
static bool rootfs;

static const char * const list_usage[] = {
	"lkvm list",
	NULL
};

static const struct option list_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('i', "run", &run, "List running instances"),
	OPT_BOOLEAN('r', "rootfs", &rootfs, "List rootfs instances"),
	OPT_END()
};

#define KVM_INSTANCE_RUNNING	"running"
#define KVM_INSTANCE_PAUSED	"paused"
#define KVM_INSTANCE_SHUTOFF	"shut off"

void kvm_list_help(void)
{
	usage_with_options(list_usage, list_options);
}

static pid_t get_pid(int sock)
{
	pid_t pid;
	int r;

	r = kvm_ipc__send(sock, KVM_IPC_PID);
	if (r < 0)
		return r;

	r = read(sock, &pid, sizeof(pid));
	if (r < 0)
		return r;

	return pid;
}

int get_vmstate(int sock)
{
	int vmstate;
	int r;

	r = kvm_ipc__send(sock, KVM_IPC_VMSTATE);
	if (r < 0)
		return r;

	r = read(sock, &vmstate, sizeof(vmstate));
	if (r < 0)
		return r;

	return vmstate;

}

static int print_guest(const char *name, int sock)
{
	pid_t pid;
	int vmstate;

	pid = get_pid(sock);
	vmstate = get_vmstate(sock);

	if ((int)pid < 0 || vmstate < 0)
		return -1;

	if (vmstate == KVM_VMSTATE_PAUSED)
		printf("%5d %-20s %s\n", pid, name, KVM_INSTANCE_PAUSED);
	else
		printf("%5d %-20s %s\n", pid, name, KVM_INSTANCE_RUNNING);

	return 0;
}

static int kvm_list_running_instances(void)
{
	return kvm__enumerate_instances(print_guest);
}

static int kvm_list_rootfs(void)
{
	DIR *dir;
	struct dirent *dirent;

	dir = opendir(kvm__get_dir());
	if (dir == NULL)
		return -1;

	while ((dirent = readdir(dir))) {
		if (dirent->d_type == DT_DIR &&
			strcmp(dirent->d_name, ".") &&
			strcmp(dirent->d_name, ".."))
			printf("%5s %-20s %s\n", "", dirent->d_name, KVM_INSTANCE_SHUTOFF);
	}

	return 0;
}

static void parse_setup_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, list_options, list_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_list_help();
	}
}

int kvm_cmd_list(int argc, const char **argv, const char *prefix)
{
	int r;

	parse_setup_options(argc, argv);

	if (!run && !rootfs)
		run = rootfs = true;

	printf("%6s %-20s %s\n", "PID", "NAME", "STATE");
	printf("------------------------------------\n");

	if (run) {
		r = kvm_list_running_instances();
		if (r < 0)
			perror("Error listing instances");
	}

	if (rootfs) {
		r = kvm_list_rootfs();
		if (r < 0)
			perror("Error listing rootfs");
	}

	return 0;
}
