#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-debug.h>
#include <kvm/kvm.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

int kvm_cmd_debug(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		die("Usage: kvm debug [instance name]\n");

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGQUIT);
}
