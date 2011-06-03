#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <assert.h>

/* user defined header files */
#include "kvm/kvm-debug.h"
#include "kvm/kvm-pause.h"
#include "kvm/kvm-help.h"
#include "kvm/kvm-cmd.h"
#include "kvm/kvm-run.h"

struct cmd_struct kvm_commands[] = {
	{ "pause", kvm_cmd_pause, NULL,         0 },
	{ "debug", kvm_cmd_debug, NULL,         0 },
	{ "help",  kvm_cmd_help,  NULL,         0 },
	{ "run",   kvm_cmd_run,   kvm_run_help, 0 },
	{ NULL,    NULL,          NULL,         0 },
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

	if (!argv || !*argv) {
		p = kvm_get_command(command, "help");
		assert(p);
		return p->fn(argc, argv, prefix);
	}

	p = kvm_get_command(command, argv[0]);
	if (!p) {
		p = kvm_get_command(command, "help");
		assert(p);
		p->fn(0, NULL, prefix);
		return EINVAL;
	}

	return p->fn(argc - 1, &argv[1], prefix);
}
