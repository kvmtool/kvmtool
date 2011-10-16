#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-resume.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

struct resume_cmd {
	u32 type;
	u32 len;
};

static bool all;
static int instance;
static const char *instance_name;

static const char * const resume_usage[] = {
	"kvm resume [--all] [-n name]",
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
	struct resume_cmd cmd = {KVM_IPC_RESUME, 0};
	int r;

	r = write(sock, &cmd, sizeof(cmd));
	if (r < 0)
		return r;

	return 0;
}

int kvm_cmd_resume(int argc, const char **argv, const char *prefix)
{
	parse_resume_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_resume);

	if (instance_name == NULL &&
	    instance == 0)
		kvm_resume_help();

	if (instance_name)
		instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	return do_resume(instance_name, instance);
}
