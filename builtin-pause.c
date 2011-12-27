#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-pause.h>
#include <kvm/builtin-list.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool all;
static const char *instance_name;

static const char * const pause_usage[] = {
	"lkvm pause [--all] [-n name]",
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
	int r;
	int vmstate;

	vmstate = get_vmstate(sock);
	if (vmstate < 0)
		return vmstate;
	if (vmstate == KVM_VMSTATE_PAUSED) {
		printf("Guest %s is already paused.\n", name);
		return 0;
	}

	r = kvm_ipc__send(sock, KVM_IPC_PAUSE);
	if (r)
		return r;

	printf("Guest %s paused\n", name);

	return 0;
}

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	int instance;
	int r;

	parse_pause_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_pause);

	if (instance_name == NULL)
		kvm_pause_help();

	instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	r = do_pause(instance_name, instance);

	close(instance);

	return r;
}
