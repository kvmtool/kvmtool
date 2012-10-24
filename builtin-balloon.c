#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-balloon.h>
#include <kvm/parse-options.h>
#include <kvm/kvm.h>
#include <kvm/kvm-ipc.h>

static const char *instance_name;
static u64 inflate;
static u64 deflate;

static const char * const balloon_usage[] = {
	"lkvm balloon [-n name] [-p pid] [-i amount] [-d amount]",
	NULL
};

static const struct option balloon_options[] = {
	OPT_GROUP("Instance options:"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_GROUP("Balloon options:"),
	OPT_U64('i', "inflate", &inflate, "Amount to inflate (in MB)"),
	OPT_U64('d', "deflate", &deflate, "Amount to deflate (in MB)"),
	OPT_END(),
};

void kvm_balloon_help(void)
{
	usage_with_options(balloon_usage, balloon_options);
}

static void parse_balloon_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, balloon_options, balloon_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_balloon_help();
	}
}

int kvm_cmd_balloon(int argc, const char **argv, const char *prefix)
{
	int instance;
	int r;
	int amount;

	parse_balloon_options(argc, argv);

	if (inflate == 0 && deflate == 0)
		kvm_balloon_help();

	if (instance_name == NULL)
		kvm_balloon_help();

	instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	if (inflate)
		amount = inflate;
	else if (deflate)
		amount = -deflate;
	else
		kvm_balloon_help();

	r = kvm_ipc__send_msg(instance, KVM_IPC_BALLOON,
			sizeof(amount), (u8 *)&amount);

	close(instance);

	if (r < 0)
		return -1;

	return 0;
}
