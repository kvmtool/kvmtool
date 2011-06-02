#include <stdio.h>
#include <string.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-debug.h>

int kvm_cmd_debug(int argc, const char **argv, const char *prefix)
{
	return system("kill -3 $(pidof kvm)");
}
