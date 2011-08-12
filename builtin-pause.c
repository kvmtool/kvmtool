#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-pause.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

static const char * const pause_usage[] = {
	"kvm pause <instance name>",
	NULL
};

static const struct option pause_options[] = {
	OPT_END()
};

void kvm_pause_help(void)
{
	usage_with_options(pause_usage, pause_options);
}

static int do_pause(const char *name, int pid)
{
	return kill(pid, SIGUSR2);
}

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		kvm_pause_help();

	if (strcmp(argv[0], "all") == 0) {
		return kvm__enumerate_instances(do_pause);
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGUSR2);
}
