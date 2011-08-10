#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-version.h>
#include <kvm/kvm.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

int kvm_cmd_version(int argc, const char **argv, const char *prefix)
{
	printf("kvm tool %s\n", KVMTOOLS_VERSION);

	return 0;
}
