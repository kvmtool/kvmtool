#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-pause.h>
#include <kvm/kvm.h>

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		die("Usage: kvm debug [instance name]\n");

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGUSR2);
}
