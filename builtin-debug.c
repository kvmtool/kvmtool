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
static int nmi = -1;
static bool dump;
static const char *instance_name;
static const char *sysrq;

static const char * const debug_usage[] = {
	"lkvm debug [--all] [-n name] [-d] [-m vcpu]",
	NULL
};

static const struct option debug_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('d', "dump", &dump, "Generate a debug dump from guest"),
	OPT_INTEGER('m', "nmi", &nmi, "Generate NMI on VCPU"),
	OPT_STRING('s', "sysrq", &sysrq, "sysrq", "Inject a sysrq"),
	OPT_GROUP("Instance options:"),
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
	struct debug_cmd_params cmd = {.dbg_type = 0};
	int r;

	if (dump)
		cmd.dbg_type |= KVM_DEBUG_CMD_TYPE_DUMP;

	if (nmi != -1) {
		cmd.dbg_type |= KVM_DEBUG_CMD_TYPE_NMI;
		cmd.cpu = nmi;
	}

	if (sysrq) {
		cmd.dbg_type |= KVM_DEBUG_CMD_TYPE_SYSRQ;
		cmd.sysrq = sysrq[0];
	}

	r = kvm_ipc__send_msg(sock, KVM_IPC_DEBUG, sizeof(cmd), (u8 *)&cmd);
	if (r < 0)
		return r;

	if (!dump)
		return 0;

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
	int instance;
	int r;

	if (all)
		return kvm__enumerate_instances(do_debug);

	if (instance_name == NULL)
		kvm_debug_help();

	instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	r = do_debug(instance_name, instance);

	close(instance);

	return r;
}
