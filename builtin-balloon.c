#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-balloon.h>
#include <kvm/parse-options.h>
#include <kvm/kvm.h>

static const char * const balloon_usage[] = {
	"kvm balloon {inflate|deflate} <size in MiB> <instance name>",
	NULL
};

static const struct option balloon_options[] = {
	OPT_END()
};

void kvm_balloon_help(void)
{
	usage_with_options(balloon_usage, balloon_options);
}

int kvm_cmd_balloon(int argc, const char **argv, const char *prefix)
{
	int pid;
	int amount, i;
	int inflate = 0;

	if (argc != 3)
		kvm_balloon_help();

	pid = kvm__get_pid_by_instance(argv[2]);
	if (pid < 0)
		die("Failed locating instance name");

	if (strcmp(argv[0], "inflate") == 0)
		inflate = 1;
	else if (strcmp(argv[0], "deflate"))
		die("command can be either 'inflate' or 'deflate'");

	amount = atoi(argv[1]);

	for (i = 0; i < amount; i++)
		kill(pid, inflate ? SIGKVMADDMEM : SIGKVMDELMEM);

	return 0;
}
