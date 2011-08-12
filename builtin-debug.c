#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-debug.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static const char * const debug_usage[] = {
	"kvm debug <instance name>",
	NULL
};

static const struct option debug_options[] = {
	OPT_END()
};

void kvm_debug_help(void)
{
	usage_with_options(debug_usage, debug_options);
}

static int do_debug(const char *name, int pid)
{
	return kill(pid, SIGQUIT);
}

int kvm_cmd_debug(int argc, const char **argv, const char *prefix)
{
	int pid;

	if (argc != 1)
		kvm_debug_help();

	if (strcmp(argv[0], "all") == 0) {
		return kvm__enumerate_instances(do_debug);
	}

	pid = kvm__get_pid_by_instance(argv[0]);
	if (pid < 0)
		die("Failed locating instance name");

	return kill(pid, SIGQUIT);
}
