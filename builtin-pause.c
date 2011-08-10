#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-pause.h>
#include <kvm/kvm.h>

static int do_pause(const char *name, int pid)
{
	return kill(pid, SIGUSR2);
}

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		die("Usage: kvm pause [instance name]\n");

	if (strcmp(argv[0], "all") == 0) {
		return kvm__enumerate_instances(do_pause);
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGUSR2);
}
