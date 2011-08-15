#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-debug.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool all;
static pid_t instance_pid;
static const char *instance_name;

static const char * const debug_usage[] = {
	"kvm debug [--all] [-n name] [-p pid]",
	NULL
};

static const struct option debug_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Debug all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_INTEGER('p', "pid", &instance_pid, "Instance pid"),
	OPT_END()
};

static void parse_debug_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, debug_options, debug_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_debug_help();
	}
}

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
	parse_debug_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_debug);

	if (instance_name == NULL &&
	    instance_pid == 0)
		kvm_debug_help();

	if (instance_name)
		instance_pid = kvm__get_pid_by_instance(instance_name);

	if (instance_pid <= 0)
		die("Failed locating instance");

	return kill(instance_pid, SIGQUIT);
}
