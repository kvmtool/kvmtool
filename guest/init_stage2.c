/*
 * This is a stage 2 of the init. This part should do all the heavy
 * lifting such as setting up the console and calling /bin/sh.
 */
#include <sys/mount.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <linux/reboot.h>

static int run_process(char *filename)
{
	char *new_argv[] = { filename, NULL };
	char *new_env[] = { "TERM=linux", NULL };

	return execve(filename, new_argv, new_env);
}

static int run_process_sandbox(char *filename)
{
	char *new_argv[] = { filename, "/virt/sandbox.sh", NULL };
	char *new_env[] = { "TERM=linux", NULL };

	return execve(filename, new_argv, new_env);
}

int main(int argc, char *argv[])
{
	pid_t child;
	int status;

	/* get session leader */
	setsid();

	/* set controlling terminal */
	ioctl(0, TIOCSCTTY, 1);

	child = fork();
	if (child < 0) {
		printf("Fatal: fork() failed with %d\n", child);
		return 0;
	} else if (child == 0) {
		if (access("/virt/sandbox.sh", R_OK) == 0)
			run_process_sandbox("/bin/sh");
		else
			run_process("/bin/sh");
	} else {
		waitpid(child, &status, 0);
	}

	reboot(LINUX_REBOOT_CMD_RESTART);

	return 0;
}
