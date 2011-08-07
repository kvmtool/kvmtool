#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-resume.h>
#include <kvm/kvm.h>

static void do_resume(const char *name, int pid)
{
	kill(pid, SIGKVMRESUME);
}

int kvm_cmd_resume(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		die("Usage: kvm resume [instance name]\n");

	if (strcmp(argv[0], "all") == 0) {
		kvm__enumerate_instances(do_resume);
		return 0;
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGKVMRESUME);
}
