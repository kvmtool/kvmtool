#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stat.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool mem;
static bool all;
static pid_t instance_pid;
static const char *instance_name;

static const char * const stat_usage[] = {
	"kvm stat [command] [--all] [-n name] [-p pid]",
	NULL
};

static const struct option stat_options[] = {
	OPT_GROUP("Commands options:"),
	OPT_BOOLEAN('m', "memory", &mem, "Display memory statistics"),
	OPT_GROUP("Instance options:"),
	OPT_BOOLEAN('a', "all", &all, "All instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_INTEGER('p', "pid", &instance_pid, "Instance pid"),
	OPT_END()
};

static void parse_stat_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, stat_options, stat_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_stat_help();
	}
}

void kvm_stat_help(void)
{
	usage_with_options(stat_usage, stat_options);
}

static int do_memstat(const char *name, int pid)
{
	printf("Sending memstat command to %s, output should be on the targets' terminal.\n", name);
	return kill(pid, SIGKVMMEMSTAT);
}

int kvm_cmd_stat(int argc, const char **argv, const char *prefix)
{
	parse_stat_options(argc, argv);

	if (!mem)
		usage_with_options(stat_usage, stat_options);

	if (mem && all)
		return kvm__enumerate_instances(do_memstat);

	if (instance_name == NULL &&
	    instance_pid == 0)
		kvm_stat_help();

	if (instance_name)
		instance_pid = kvm__get_pid_by_instance(instance_name);

	if (instance_pid <= 0)
		die("Failed locating instance");

	if (mem) {
		printf("Sending memstat command to designated instance, output should be on the targets' terminal.\n");

		return kill(instance_pid, SIGKVMMEMSTAT);
	}

	return 0;
}
