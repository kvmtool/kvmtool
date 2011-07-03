#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-list.h>
#include <kvm/kvm.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

static void print_guest(const char *name, int pid)
{
	printf("%s (PID: %d)\n", name, pid);
}

int kvm_cmd_list(int argc, const char **argv, const char *prefix)
{
	kvm__enumerate_instances(print_guest);

	return 0;
}
