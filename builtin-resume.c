#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-resume.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static bool all;
static u64 instance_pid;
static const char *instance_name;

static const char * const resume_usage[] = {
	"kvm resume [--all] [-n name] [-p pid]",
	NULL
};

static const struct option resume_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('a', "all", &all, "Resume all instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_U64('p', "pid", &instance_pid, "Instance pid"),
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

static int do_resume(const char *name, int pid)
{
	return kill(pid, SIGKVMRESUME);
}

int kvm_cmd_resume(int argc, const char **argv, const char *prefix)
{
	parse_resume_options(argc, argv);

	if (all)
		return kvm__enumerate_instances(do_resume);

	if (instance_name == NULL &&
	    instance_pid == 0)
		kvm_resume_help();

	if (instance_name)
		instance_pid = kvm__get_pid_by_instance(instance_name);

	if (instance_pid <= 0)
		die("Failed locating instance");

	return kill(instance_pid, SIGKVMRESUME);
}
