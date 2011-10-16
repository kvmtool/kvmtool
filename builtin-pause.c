#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-pause.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

struct pause_cmd {
	u32 type;
	u32 len;
};

static bool all;
static int instance;
static const char *instance_name;

static const char * const pause_usage[] = {
	"kvm pause [--all] [-n name]",
	NULL
};

static const struct option pause_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Pause all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
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

static int do_pause(const char *name, int sock)
{
	struct pause_cmd cmd = {KVM_IPC_PAUSE, 0};
	int r;

	r = write(sock, &cmd, sizeof(cmd));
	if (r < 0)
		return r;

	return 0;
}

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	parse_pause_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_pause);

	if (instance_name == NULL &&
	    instance == 0)
		kvm_pause_help();

	if (instance_name)
		instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	return do_pause(instance_name, instance);
}
