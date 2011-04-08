#include <stdio.h>

/* user defined header files */
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-help.h>
#include <kvm/kvm-run.h>

static int handle_kvm_command(int argc, char **argv)
{
	struct cmd_struct command[] = {
		{ "help",  kvm_cmd_help,  0 },
		{ "run",   kvm_cmd_run,   0 },
		{ NULL,    NULL,          0 },
	};

	return handle_command(command, argc, (const char **) &argv[0]);
}

int main(int argc, char *argv[])
{
	return handle_kvm_command(argc - 1, &argv[1]);
}
