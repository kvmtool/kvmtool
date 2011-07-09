#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-balloon.h>
#include <kvm/kvm.h>

int kvm_cmd_balloon(int argc, const char **argv, const char *prefix)
{
	int pid;
	int amount, i;
	int inflate = 0;

	if (argc != 3)
		die("Usage: kvm balloon [command] [instance name] [amount]\n");

	pid = kvm__get_pid_by_instance(argv[1]);
	if (pid < 0)
		die("Failed locating instance name");

	if (strcmp(argv[0], "inflate") == 0)
		inflate = 1;
	else if (strcmp(argv[0], "deflate"))
		die("command can be either 'inflate' or 'deflate'");

	amount = atoi(argv[2]);

	for (i = 0; i < amount; i++)
		kill(pid, inflate ? SIGKVMADDMEM : SIGKVMDELMEM);

	return 0;
}
