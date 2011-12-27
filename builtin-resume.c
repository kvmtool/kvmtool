#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-resume.h>
#include <kvm/builtin-list.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool all;
static const char *instance_name;

static const char * const resume_usage[] = {
	"lkvm resume [--all] [-n name]",
	NULL
};

static const struct option resume_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Resume all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_END()
};

static void parse_resume_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, resume_options, resume_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_resume_help();
	}
}

void kvm_resume_help(void)
{
	usage_with_options(resume_usage, resume_options);
}

static int do_resume(const char *name, int sock)
{
	int r;
	int vmstate;

	vmstate = get_vmstate(sock);
	if (vmstate < 0)
		return vmstate;
	if (vmstate == KVM_VMSTATE_RUNNING) {
		printf("Guest %s is still running.\n", name);
		return 0;
	}

	r = kvm_ipc__send(sock, KVM_IPC_RESUME);
	if (r)
		return r;

	printf("Guest %s resumed\n", name);

	return 0;
}

int kvm_cmd_resume(int argc, const char **argv, const char *prefix)
{
	int instance;
	int r;

	parse_resume_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_resume);

	if (instance_name == NULL)
		kvm_resume_help();

	instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	r = do_resume(instance_name, instance);

	close(instance);

	return r;
}
