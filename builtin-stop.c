#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stop.h>
#include <kvm/kvm.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static void do_stop(const char *name, int pid)
{
	kill(pid, SIGKVMSTOP);
}

int kvm_cmd_stop(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		die("Usage: kvm stop [instance name]\n");

	if (strcmp(argv[0], "all") == 0) {
		kvm__enumerate_instances(do_stop);
		return 0;
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGKVMSTOP);
}
