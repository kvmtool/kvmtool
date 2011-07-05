#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-list.h>
#include <kvm/kvm.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#define PROCESS_NAME "kvm"

static void print_guest(const char *name, int pid)
{
	char proc_name[PATH_MAX];
	char comm[sizeof(PROCESS_NAME)];
	int fd;

	sprintf(proc_name, "/proc/%d/comm", pid);
	fd = open(proc_name, O_RDONLY);
	if (fd <= 0)
		goto cleanup;
	if (read(fd, comm, sizeof(PROCESS_NAME)) == 0)
		goto cleanup;
	if (strncmp(comm, PROCESS_NAME, strlen(PROCESS_NAME)))
		goto cleanup;

	printf("%s (PID: %d)\n", name, pid);

	return;

cleanup:
	kvm__remove_pidfile(name);
}

int kvm_cmd_list(int argc, const char **argv, const char *prefix)
{
	kvm__enumerate_instances(print_guest);

	return 0;
}
