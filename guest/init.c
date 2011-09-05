/*
 * This is a simple init for shared rootfs guests. It brings up critical
 * mountpoints and then launches /bin/sh.
 */
#include <sys/mount.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

static int run_process(char *filename)
{
	char *new_argv[] = { filename, NULL };
	char *new_env[] = { NULL };

	return execve(filename, new_argv, new_env);
}

static void do_mounts(void)
{
	mount("hostfs", "/host", "9p", MS_RDONLY, "trans=virtio,version=9p2000.L");
	mount("", "/sys", "sysfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
}

int main(int argc, char *argv[])
{
	puts("Mounting...");

	do_mounts();

	puts("Setting up network...");

	system("/bin/sh virt/setnet.sh");

	puts("Starting '/bin/sh'...");

	run_process("/bin/sh");

	printf("Init failed: %s\n", strerror(errno));

	return 0;
}
