#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stop.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

struct stop_cmd {
	u32 type;
	u32 len;
};

static bool all;
static const char *instance_name;

static const char * const stop_usage[] = {
	"lkvm stop [--all] [-n name]",
	NULL
};

static const struct option stop_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Stop all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
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

static int do_stop(const char *name, int sock)
{
	struct stop_cmd cmd = {KVM_IPC_STOP, 0};
	int r;

	r = write(sock, &cmd, sizeof(cmd));
	if (r < 0)
		return r;

	return 0;
}

int kvm_cmd_stop(int argc, const char **argv, const char *prefix)
{
	int instance;
	int r;

	parse_stop_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_stop);

	if (instance_name == NULL)
		kvm_stop_help();

	instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	r = do_stop(instance_name, instance);

	close(instance);

	return r;
}
