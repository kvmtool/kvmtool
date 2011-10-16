#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stat.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

struct stat_cmd {
	u32 type;
	u32 len;
};

static bool mem;
static bool all;
static int instance;
static const char *instance_name;

static const char * const stat_usage[] = {
	"kvm stat [command] [--all] [-n name]",
	NULL
};

static const struct option stat_options[] = {
	OPT_GROUP("Commands options:"),
	OPT_BOOLEAN('m', "memory", &mem, "Display memory statistics"),
	OPT_GROUP("Instance options:"),
	OPT_BOOLEAN('a', "all", &all, "All instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
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

static int do_memstat(const char *name, int sock)
{
	struct stat_cmd cmd = {KVM_IPC_STAT, 0};
	int r;

	printf("Sending memstat command to %s, output should be on the targets' terminal.\n", name);

	r = write(sock, &cmd, sizeof(cmd));
	if (r < 0)
		return r;

	return 0;
}

int kvm_cmd_stat(int argc, const char **argv, const char *prefix)
{
	parse_stat_options(argc, argv);

	if (!mem)
		usage_with_options(stat_usage, stat_options);

	if (mem && all)
		return kvm__enumerate_instances(do_memstat);

	if (instance_name == NULL &&
	    instance == 0)
		kvm_stat_help();

	if (instance_name)
		instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	if (mem)
		return do_memstat(instance_name, instance);

	return 0;
}
