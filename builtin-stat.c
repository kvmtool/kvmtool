#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-stat.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/kvm-ipc.h>

#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <linux/virtio_balloon.h>

static bool mem;
static bool all;
static const char *instance_name;

static const char * const stat_usage[] = {
	"lkvm stat [command] [--all] [-n name]",
	NULL
};

static const struct option stat_options[] = {
	OPT_GROUP("Commands options:"),
	OPT_BOOLEAN('m', "memory", &mem, "Display memory statistics"),
	OPT_GROUP("Instance options:"),
	OPT_BOOLEAN('a', "all", &all, "All instances"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_END()
};

static void parse_stat_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, stat_options, stat_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0)
			kvm_stat_help();
	}
}

void kvm_stat_help(void)
{
	usage_with_options(stat_usage, stat_options);
}

static int do_memstat(const char *name, int sock)
{
	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];
	fd_set fdset;
	struct timeval t = { .tv_sec = 1 };
	int r;
	u8 i;

	FD_ZERO(&fdset);
	FD_SET(sock, &fdset);
	r = kvm_ipc__send(sock, KVM_IPC_STAT);
	if (r < 0)
		return r;

	r = select(1, &fdset, NULL, NULL, &t);
	if (r < 0) {
		pr_err("Could not retrieve mem stats from %s", name);
		return r;
	}
	r = read(sock, &stats, sizeof(stats));
	if (r < 0)
		return r;

	printf("\n\n\t*** Guest memory statistics ***\n\n");
	for (i = 0; i < VIRTIO_BALLOON_S_NR; i++) {
		switch (stats[i].tag) {
		case VIRTIO_BALLOON_S_SWAP_IN:
			printf("The amount of memory that has been swapped in (in bytes):");
			break;
		case VIRTIO_BALLOON_S_SWAP_OUT:
			printf("The amount of memory that has been swapped out to disk (in bytes):");
			break;
		case VIRTIO_BALLOON_S_MAJFLT:
			printf("The number of major page faults that have occurred:");
			break;
		case VIRTIO_BALLOON_S_MINFLT:
			printf("The number of minor page faults that have occurred:");
			break;
		case VIRTIO_BALLOON_S_MEMFREE:
			printf("The amount of memory not being used for any purpose (in bytes):");
			break;
		case VIRTIO_BALLOON_S_MEMTOT:
			printf("The total amount of memory available (in bytes):");
			break;
		}
		printf("%llu\n", stats[i].val);
	}
	printf("\n");

	return 0;
}

int kvm_cmd_stat(int argc, const char **argv, const char *prefix)
{
	int instance;
	int r = 0;

	parse_stat_options(argc, argv);

	if (!mem)
		usage_with_options(stat_usage, stat_options);

	if (mem && all)
		return kvm__enumerate_instances(do_memstat);

	if (instance_name == NULL)
		kvm_stat_help();

	instance = kvm__get_sock_by_instance(instance_name);

	if (instance <= 0)
		die("Failed locating instance");

	if (mem)
		r = do_memstat(instance_name, instance);

	close(instance);

	return r;
}
