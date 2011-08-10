#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-list.h>
#include <kvm/kvm.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#define PROCESS_NAME "kvm"

static int print_guest(const char *name, int pid)
{
	char proc_name[PATH_MAX];
	char *comm = NULL;
	FILE *fd;

	sprintf(proc_name, "/proc/%d/stat", pid);
	fd = fopen(proc_name, "r");
	if (fd == NULL)
		goto cleanup;
	if (fscanf(fd, "%*u (%as)", &comm) == 0)
		goto cleanup;
	if (strncmp(comm, PROCESS_NAME, strlen(PROCESS_NAME)))
		goto cleanup;

	printf("%s (PID: %d)\n", name, pid);

	free(comm);

	fclose(fd);

	return 0;

cleanup:
	if (fd)
		fclose(fd);
	if (comm)
		free(comm);

	kvm__remove_pidfile(name);
	return 0;
}

int kvm_cmd_list(int argc, const char **argv, const char *prefix)
{
	return kvm__enumerate_instances(print_guest);
}
