#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stop.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static const char * const stop_usage[] = {
	"kvm stop <instance name>",
	NULL
};

static const struct option stop_options[] = {
	OPT_END()
};

void kvm_stop_help(void)
{
	usage_with_options(stop_usage, stop_options);
}

static int do_stop(const char *name, int pid)
{
	return kill(pid, SIGKVMSTOP);
}

int kvm_cmd_stop(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		kvm_stop_help();

	if (strcmp(argv[0], "all") == 0) {
		return kvm__enumerate_instances(do_stop);
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGKVMSTOP);
}
