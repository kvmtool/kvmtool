#ifndef __KVM_CMD_H__
#define __KVM_CMD_H__

struct cmd_struct {
	const char *cmd;
	int (*fn)(int, const char **, const char *);
	void (*help)(void);
	int option;
};

extern struct cmd_struct kvm_commands[];
struct cmd_struct *kvm_get_command(struct cmd_struct *command,
                const char *cmd);

int handle_command(struct cmd_struct *command, int argc, const char **argv);

#endif
