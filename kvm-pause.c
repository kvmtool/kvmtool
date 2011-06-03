#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/kvm-pause.h>

int kvm_cmd_pause(int argc, const char **argv, const char *prefix)
{
	signal(SIGUSR2, SIG_IGN);
	return system("kill -USR2 $(pidof kvm)");
}
