#include <stdio.h>
#include <string.h>
#include <errno.h>

/* user defined header files */
#include "kvm/builtin-debug.h"
#include "kvm/builtin-pause.h"
#include "kvm/builtin-resume.h"
#include "kvm/builtin-balloon.h"
#include "kvm/builtin-list.h"
#include "kvm/builtin-version.h"
#include "kvm/builtin-setup.h"
#include "kvm/builtin-stop.h"
#include "kvm/builtin-stat.h"
#include "kvm/builtin-help.h"
#include "kvm/builtin-sandbox.h"
#include "kvm/kvm-cmd.h"
#include "kvm/builtin-run.h"
#include "kvm/util.h"

struct cmd_struct kvm_commands[] = {
	{ "pause",	kvm_cmd_pause,		kvm_pause_help,		0 },
	{ "resume",	kvm_cmd_resume,		kvm_resume_help,	0 },
	{ "debug",	kvm_cmd_debug,		kvm_debug_help,		0 },
	{ "balloon",	kvm_cmd_balloon,	kvm_balloon_help,	0 },
	{ "list",	kvm_cmd_list,		kvm_list_help,		0 },
	{ "version",	kvm_cmd_version,	NULL,			0 },
	{ "--version",	kvm_cmd_version,	NULL,			0 },
	{ "stop",	kvm_cmd_stop,		kvm_stop_help,		0 },
	{ "stat",	kvm_cmd_stat,		kvm_stat_help,		0 },
	{ "help",	kvm_cmd_help,		NULL,			0 },
	{ "setup",	kvm_cmd_setup,		kvm_setup_help,		0 },
	{ "run",	kvm_cmd_run,		kvm_run_help,		0 },
	{ "sandbox",	kvm_cmd_sandbox,	kvm_run_help,		0 },
	{ NULL,		NULL,			NULL,			0 },
};

/*
 * kvm_get_command: Searches the command in an array of the commands and
 * returns a pointer to cmd_struct if a match is found.
 *
 * Input parameters:
 * command: Array of possible commands. The last entry in the array must be
 *          NULL.
 * cmd: A string command to search in the array
 *
 * Return Value:
 * NULL: If the cmd is not matched with any of the command in the command array
 * p: Pointer to cmd_struct of the matching command
 */
struct cmd_struct *kvm_get_command(struct cmd_struct *command,
		const char *cmd)
{
	struct cmd_struct *p = command;

	while (p->cmd) {
		if (!strcmp(p->cmd, cmd))
			return p;
		p++;
	}
	return NULL;
}

int handle_command(struct cmd_struct *command, int argc, const char **argv)
{
	struct cmd_struct *p;
	const char *prefix = NULL;
	int ret = 0;

	if (!argv || !*argv) {
		p = kvm_get_command(command, "help");
		BUG_ON(!p);
		return p->fn(argc, argv, prefix);
	}

	p = kvm_get_command(command, argv[0]);
	if (!p) {
		p = kvm_get_command(command, "help");
		BUG_ON(!p);
		p->fn(0, NULL, prefix);
		return EINVAL;
	}

	ret = p->fn(argc - 1, &argv[1], prefix);
	if (ret < 0) {
		if (errno == EPERM)
			die("Permission error - are you root?");
	}

	return ret;
}
