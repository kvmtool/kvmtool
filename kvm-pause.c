#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-pause.h>
#include <kvm/kvm.h>

static void do_pause(const char *name, int pid)
{
	kill(pid, SIGUSR2);
}

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		die("Usage: kvm debug [instance name]\n");

	if (strcmp(argv[0], "all") == 0) {
		kvm__enumerate_instances(do_pause);
		return 0;
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGUSR2);
}
