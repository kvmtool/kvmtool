#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-list.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#define PROCESS_NAME "kvm"

static bool run;
static bool rootfs;

static const char * const list_usage[] = {
	"kvm list",
	NULL
};

static const struct option list_options[] = {
	OPT_GROUP("General options:"),
	OPT_BOOLEAN('i', "run", &run, "List running instances"),
	OPT_BOOLEAN('r', "rootfs", &rootfs, "List rootfs instances"),
	OPT_END()
};

void kvm_list_help(void)
{
	usage_with_options(list_usage, list_options);
}

static int print_guest(const char *name, int pid)
{
	char proc_name[PATH_MAX];
	char *comm = NULL;
	FILE *fd;

	sprintf(proc_name, "/proc/%d/stat", pid);
	fd = fopen(proc_name, "r");
	if (fd == NULL)
		goto cleanup;
	if (fscanf(fd, "%*u (%as)", &comm) == 0)
		goto cleanup;
	if (strncmp(comm, PROCESS_NAME, strlen(PROCESS_NAME)))
		goto cleanup;

	printf("%5d %s\n", pid, name);

	free(comm);

	fclose(fd);

	return 0;

cleanup:
	if (fd)
		fclose(fd);
	if (comm)
		free(comm);

	kvm__remove_pidfile(name);
	return 0;
}

static int kvm_list_running_instances(void)
{
	printf("  PID GUEST\n");

	return kvm__enumerate_instances(print_guest);
}

static int kvm_list_rootfs(void)
{
	char name[PATH_MAX];
	DIR *dir;
	struct dirent *dirent;

	snprintf(name, PATH_MAX, "%s%s", HOME_DIR, KVM_PID_FILE_PATH);
	dir = opendir(name);
	if (dir == NULL)
		return -1;

	printf("  ROOTFS\n");

	while ((dirent = readdir(dir))) {
		if (dirent->d_type == DT_DIR &&
			strcmp(dirent->d_name, ".") &&
			strcmp(dirent->d_name, ".."))
			printf("%s\n", dirent->d_name);
	}

	return 0;
}

static void parse_setup_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, list_options, list_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_list_help();
	}
}

int kvm_cmd_list(int argc, const char **argv, const char *prefix)
{
	int r;

	parse_setup_options(argc, argv);

	if (!run && !rootfs)
		kvm_list_help();

	if (run) {
		r = kvm_list_running_instances();
		if (r < 0)
			perror("Error listing instances");
	}

	if (rootfs) {
		r = kvm_list_rootfs();
		if (r < 0)
			perror("Error listing rootfs");
	}

	return 0;
}
