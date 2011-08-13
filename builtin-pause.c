#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-pause.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool all;
static u64 instance_pid;
static const char *instance_name;

static const char * const pause_usage[] = {
	"kvm pause [--all] [-n name] [-p pid]",
	NULL
};

static const struct option pause_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Pause all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_U64('p', "pid", &instance_pid, "Instance pid"),
	OPT_END()
};

static void parse_pause_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, pause_options, pause_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_pause_help();
	}
}

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
	parse_pause_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_pause);

	if (instance_name == NULL &&
	    instance_pid == 0)
		kvm_pause_help();

	if (instance_name)
		instance_pid = kvm__get_pid_by_instance(instance_name);

	if (instance_pid <= 0)
		die("Failed locating instance");

	return kill(instance_pid, SIGUSR2);
}
