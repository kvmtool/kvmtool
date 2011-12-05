/*
 * This is a stage 2 of the init. This part should do all the heavy
 * lifting such as setting up the console and calling /bin/sh.
 */
#include <sys/mount.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

static int run_process(char *filename)
{
	char *new_argv[] = { filename, NULL };
	char *new_env[] = { "TERM=linux", NULL };

	return execve(filename, new_argv, new_env);
}

int main(int argc, char *argv[])
{
	/* get session leader */
	setsid();

	/* set controlling terminal */
	ioctl(0, TIOCSCTTY, 1);

	puts("Starting '/bin/sh'...");

	run_process("/bin/sh");

	printf("Init failed: %s\n", strerror(errno));

	return 0;
}
