#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-debug.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>
#include <kvm/read-write.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

#define BUFFER_SIZE 100

static bool all;
static int instance;
static const char *instance_name;

struct debug_cmd {
	u32 type;
	u32 len;
};

static const char * const debug_usage[] = {
	"kvm debug [--all] [-n name]",
	NULL
};

static const struct option debug_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Debug all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
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

static int do_debug(const char *name, int sock)
{
	char buff[BUFFER_SIZE];
	struct debug_cmd cmd = {KVM_IPC_DEBUG, 0};
	int r;

	r = xwrite(sock, &cmd, sizeof(cmd));
	if (r < 0)
		return r;

	do {
		r = xread(sock, buff, BUFFER_SIZE);
		if (r < 0)
			return 0;
		printf("%.*s", r, buff);
	} while (r > 0);

	return 0;
}

int kvm_cmd_debug(int argc, const char **argv, const char *prefix)
{
	parse_debug_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_debug);

	if (instance_name == NULL &&
	    instance == 0)
		kvm_debug_help();

	if (instance_name)
		instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	return do_debug(instance_name, instance);
}
