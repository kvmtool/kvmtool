#include <stdio.h>
#include <string.h>

/* user defined headers */
#include <common-cmds.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-help.h>
#include <kvm/kvm.h>


const char kvm_usage_string[] =
	"lkvm COMMAND [ARGS]";

const char kvm_more_info_string[] =
	"See 'lkvm help COMMAND' for more information on a specific command.";


static void list_common_cmds_help(void)
{
	unsigned int i, longest = 0;

	for (i = 0; i < ARRAY_SIZE(common_cmds); i++) {
		if (longest < strlen(common_cmds[i].name))
			longest = strlen(common_cmds[i].name);
	}

	puts(" The most commonly used lkvm commands are:");
	for (i = 0; i < ARRAY_SIZE(common_cmds); i++) {
		printf("   %-*s   ", longest, common_cmds[i].name);
		puts(common_cmds[i].help);
	}
}

static void kvm_help(void)
{
	printf("\n To start a simple non-privileged shell run '%s run'\n\n"
		"usage: %s\n\n", KVM_BINARY_NAME, kvm_usage_string);
	list_common_cmds_help();
	printf("\n %s\n\n", kvm_more_info_string);
}


static void help_cmd(const char *cmd)
{
	struct cmd_struct *p;
	p = kvm_get_command(kvm_commands, cmd);
	if (!p)
		kvm_help();
	else if (p->help)
		p->help();
}

int kvm_cmd_help(int argc, const char **argv, const char *prefix)
{
	if (!argv || !*argv) {
		kvm_help();
		return 0;
	}
	help_cmd(argv[0]);
	return 0;
}
