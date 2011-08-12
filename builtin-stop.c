#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stop.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool all;
static u64 instance_pid;
static const char *instance_name;

static const char * const stop_usage[] = {
	"kvm stop [--all] [-n name] [-p pid]",
	NULL
};

static const struct option stop_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Stop all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_U64('p', "pid", &instance_pid, "Instance pid"),
	OPT_END()
};

static void parse_stop_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, stop_options, stop_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_stop_help();
	}
}

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
	parse_stop_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_stop);

	if (instance_name == NULL &&
	    instance_pid == 0)
		kvm_stop_help();

	if (instance_name)
		instance_pid = kvm__get_pid_by_instance(argv[0]);

	if (instance_pid <= 0)
		die("Failed locating instance");

	return kill(instance_pid, SIGKVMSTOP);
}
