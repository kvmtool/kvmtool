#ifndef __KVM_CMD_H__
#define __KVM_CMD_H__

struct cmd_struct {
	const char *cmd;
	int (*fn)(int, const char **, const char *);
	int option;
};

int handle_command(struct cmd_struct *command, int argc, const char **argv);

#endif
