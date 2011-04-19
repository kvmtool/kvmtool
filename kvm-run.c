#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>

/* user defined header files */
#include <linux/types.h>
#include <kvm/kvm.h>
#include <kvm/kvm-cpu.h>
#include <kvm/8250-serial.h>
#include <kvm/virtio-blk.h>
#include <kvm/virtio-net.h>
#include <kvm/virtio-console.h>
#include <kvm/disk-image.h>
#include <kvm/util.h>
#include <kvm/pci.h>
#include <kvm/term.h>
#include <kvm/ioport.h>

/* header files for gitish interface  */
#include <kvm/kvm-run.h>
#include <kvm/parse-options.h>

#define DEFAULT_KVM_DEV		"/dev/kvm"
#define DEFAULT_CONSOLE		"serial"
#define DEFAULT_NETWORK		"virtio"
#define DEFAULT_HOST_ADDR	"192.168.33.2"
#define DEFAULT_GUEST_MAC	"00:11:22:33:44:55"
#define DEFAULT_SCRIPT		"none"

#define MB_SHIFT		(20)
#define MIN_RAM_SIZE_MB		(64ULL)
#define MIN_RAM_SIZE_BYTE	(MIN_RAM_SIZE_MB << MB_SHIFT)

#define KVM_NR_CPUS		(255)

static struct kvm *kvm;
static struct kvm_cpu *kvm_cpus[KVM_NR_CPUS];
static __thread struct kvm_cpu *current_kvm_cpu;

static void handle_sigquit(int sig)
{
	serial8250__inject_sysrq(kvm);
}

static void handle_sigalrm(int sig)
{
	serial8250__inject_interrupt(kvm);
	virtio_console__inject_interrupt(kvm);
}

static u64 ram_size = MIN_RAM_SIZE_MB;
static const char *kernel_cmdline;
static const char *kernel_filename;
static const char *initrd_filename;
static const char *image_filename;
static const char *console;
static const char *kvm_dev;
static const char *network;
static const char *host_ip_addr;
static const char *guest_mac;
static const char *script;
static bool single_step;
static bool readonly_image;
extern bool ioport_debug;
extern int  active_console;

static int nrcpus = 1;

static const char * const run_usage[] = {
	"kvm run [<options>] [<kernel image>]",
	NULL
};

static const struct option options[] = {
	OPT_GROUP("Basic options:"),
	OPT_INTEGER('\0', "cpus", &nrcpus, "Number of CPUs"),
	OPT_U64('m', "mem", &ram_size, "Virtual machine memory size in MiB."),
	OPT_STRING('i', "image", &image_filename, "image", "Disk image"),
	OPT_BOOLEAN('\0', "readonly", &readonly_image,
			"Don't write changes back to disk image"),
	OPT_STRING('c', "console", &console, "serial or virtio",
			"Console to use"),

	OPT_GROUP("Kernel options:"),
	OPT_STRING('k', "kernel", &kernel_filename, "kernel",
			"Kernel to boot in virtual machine"),
	OPT_STRING('r', "initrd", &initrd_filename, "initrd",
			"Initial RAM disk image"),
	OPT_STRING('p', "params", &kernel_cmdline, "params",
			"Kernel command line arguments"),

	OPT_GROUP("Networking options:"),
	OPT_STRING('n', "network", &network, "virtio",
			"Network to use"),
	OPT_STRING('\0', "host-ip-addr", &host_ip_addr, "a.b.c.d",
			"Assign this address to the host side networking"),
	OPT_STRING('\0', "guest-mac", &guest_mac, "aa:bb:cc:dd:ee:ff",
			"Assign this address to the guest side NIC"),
	OPT_STRING('\0', "tapscript", &script, "Script path",
			 "Assign a script to process created tap device"),
	OPT_GROUP("Debug options:"),
	OPT_STRING('d', "kvm-dev", &kvm_dev, "kvm-dev", "KVM device file"),
	OPT_BOOLEAN('s', "single-step", &single_step,
			"Enable single stepping"),
	OPT_BOOLEAN('g', "ioport-debug", &ioport_debug,
			"Enable ioport debugging"),
	OPT_END()
};

static void *kvm_cpu_thread(void *arg)
{
	current_kvm_cpu		= arg;

	if (kvm_cpu__start(current_kvm_cpu))
		goto panic_kvm;

	kvm_cpu__delete(current_kvm_cpu);

	return (void *) (intptr_t) 0;

panic_kvm:
	fprintf(stderr, "KVM exit reason: %" PRIu32 " (\"%s\")\n",
		current_kvm_cpu->kvm_run->exit_reason,
		kvm_exit_reasons[current_kvm_cpu->kvm_run->exit_reason]);
	if (current_kvm_cpu->kvm_run->exit_reason == KVM_EXIT_UNKNOWN)
		fprintf(stderr, "KVM exit code: 0x%Lu\n",
			current_kvm_cpu->kvm_run->hw.hardware_exit_reason);
	disk_image__close(kvm->disk_image);
	kvm_cpu__show_registers(current_kvm_cpu);
	kvm_cpu__show_code(current_kvm_cpu);
	kvm_cpu__show_page_tables(current_kvm_cpu);

	kvm_cpu__delete(current_kvm_cpu);

	return (void *) (intptr_t) 1;
}

static char kernel[PATH_MAX];
const char *host_kernels[] = {
	"/boot/vmlinuz",
	"/boot/bzImage",
	NULL
};
const char *default_kernels[] = {
	"./bzImage",
	"../../arch/x86/boot/bzImage",
	NULL
};

static void kernel_usage_with_options(void)
{
	const char **k;
	struct utsname uts;

	fprintf(stderr, "Fatal: could not find default kernel image in:\n");
	k = &default_kernels[0];
	while (*k) {
		fprintf(stderr, "\t%s\n", *k);
		k++;
	}

	if (uname(&uts) < 0)
		return;

	k = &host_kernels[0];
	while (*k) {
		if (snprintf(kernel, PATH_MAX, "%s-%s", *k, uts.release) < 0)
			return;
		fprintf(stderr, "\t%s\n", kernel);
		k++;
	}
	usage_with_options(run_usage, options);
}

static const char *find_kernel(void)
{
	const char **k;
	struct stat st;
	struct utsname uts;

	k = &default_kernels[0];
	while (*k) {
		if (stat(*k, &st) < 0 || !S_ISREG(st.st_mode)) {
			k++;
			continue;
		}
		strncpy(kernel, *k, PATH_MAX);
		return kernel;
	}

	if (uname(&uts) < 0)
		return NULL;

	k = &host_kernels[0];
	while (*k) {
		if (snprintf(kernel, PATH_MAX, "%s-%s", *k, uts.release) < 0)
			return NULL;

		if (stat(kernel, &st) < 0 || !S_ISREG(st.st_mode)) {
			k++;
			continue;
		}
		return kernel;

	}
	return NULL;
}

static int root_device(char *dev, long *part)
{
	FILE   *fp;
	char   *line;
	int    tmp;
	size_t nr_read;
	char   device[PATH_MAX];
	char   mnt_pt[PATH_MAX];
	char   resolved_path[PATH_MAX];
	char   *p;
	struct stat st;

	fp = fopen("/proc/mounts", "r");
	if (!fp)
		return -1;

	line = NULL;
	tmp  = 0;
	while (!feof(fp)) {
		if (getline(&line, &nr_read, fp) < 0)
			break;
		sscanf(line, "%s %s", device, mnt_pt);
		if (!strncmp(device, "/dev", 4) && !strcmp(mnt_pt, "/")) {
			tmp = 1;
			break;
		}
	}
	fclose(fp);
	free(line);

	if (!tmp)
		return -1;

	/* get the absolute path */
	if (!realpath(device, resolved_path))
		return -1;

	/* find the partition number */
	p = resolved_path;
	while (*p) {
		if (isdigit(*p)) {
			strncpy(dev, resolved_path, p - resolved_path);
			*part = atol(p);
			break;
		}
		p++;
	}

	/* verify the device path */
	if (stat(dev, &st) < 0)
		return -1;
	return 0;
}

static char *host_image(char *cmd_line, size_t size)
{
	char *t;
	char device[PATH_MAX];
	long part = 0;

	t = malloc(PATH_MAX);
	if (!t)
		return NULL;

	/* check for the root file system */
	if (root_device(device, &part) < 0) {
		free(t);
		return NULL;
	}
	strncpy(t, device, PATH_MAX);
	if (!strstr(cmd_line, "root=")) {
		char tmp[PATH_MAX];
		snprintf(tmp, sizeof(tmp), "root=/dev/vda%ld rw ", part);
		strlcat(cmd_line, tmp, size);
	}
	return t;
}

int kvm_cmd_run(int argc, const char **argv, const char *prefix)
{
	static char real_cmdline[2048];
	int exit_code = 0;
	int i;
	struct virtio_net_parameters net_params;
	char *hi;

	signal(SIGALRM, handle_sigalrm);
	signal(SIGQUIT, handle_sigquit);

	while (argc != 0) {
		argc = parse_options(argc, argv, options, run_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0) {
			if (kernel_filename) {
				fprintf(stderr, "Cannot handle parameter: "
						"%s\n", argv[0]);
				usage_with_options(run_usage, options);
				return EINVAL;
			}
			/* first unhandled parameter is treated as a kernel
			   image
			 */
			kernel_filename = argv[0];
			argv++;
			argc--;
		}

	}

	if (!kernel_filename)
		kernel_filename = find_kernel();

	if (!kernel_filename) {
		kernel_usage_with_options();
		return EINVAL;
	}

	if (nrcpus < 1 || nrcpus > KVM_NR_CPUS)
		die("Number of CPUs %d is out of [1;%d] range", nrcpus, KVM_NR_CPUS);

	/* FIXME: Remove as only SMP gets fully supported */
	if (nrcpus > 1)
		warning("Limiting CPUs to 1, true SMP is not yet implemented");

	if (ram_size < MIN_RAM_SIZE_MB)
		die("Not enough memory specified: %lluMB (min %lluMB)", ram_size, MIN_RAM_SIZE_MB);

	ram_size <<= MB_SHIFT;

	if (!kvm_dev)
		kvm_dev = DEFAULT_KVM_DEV;

	if (!console)
		console = DEFAULT_CONSOLE;

	if (!strncmp(console, "virtio", 6))
		active_console  = CONSOLE_VIRTIO;
	else
		active_console  = CONSOLE_8250;

	if (!host_ip_addr)
		host_ip_addr = DEFAULT_HOST_ADDR;

	if (!guest_mac)
		guest_mac = DEFAULT_GUEST_MAC;

	if (!script)
		script = DEFAULT_SCRIPT;

	term_init();

	kvm = kvm__init(kvm_dev, ram_size);

	memset(real_cmdline, 0, sizeof(real_cmdline));
	strcpy(real_cmdline, "notsc nolapic noacpi pci=conf1 console=ttyS0 ");
	if (kernel_cmdline)
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));

	hi = NULL;
	if (!image_filename) {
		hi = host_image(real_cmdline, sizeof(real_cmdline));
		if (hi) {
			image_filename = hi;
			readonly_image = true;
		}
	}

	if (!strstr(real_cmdline, "root="))
		strlcat(real_cmdline, "root=/dev/vda rw ", sizeof(real_cmdline));

	if (image_filename) {
		kvm->disk_image	= disk_image__open(image_filename, readonly_image);
		if (!kvm->disk_image)
			die("unable to load disk image %s", image_filename);
	}
	free(hi);

	if (!kvm__load_kernel(kvm, kernel_filename, initrd_filename,
				real_cmdline))
		die("unable to load kernel %s", kernel_filename);

	ioport__setup_legacy();

	kvm__setup_bios(kvm);

	serial8250__init(kvm);

	pci__init();

	virtio_blk__init(kvm);

	virtio_console__init(kvm);

	if (!network)
		network = DEFAULT_NETWORK;

	if (!strncmp(network, "virtio", 6)) {
		net_params = (struct virtio_net_parameters) {
			.host_ip = host_ip_addr,
			.self = kvm,
			.script = script
		};
		sscanf(guest_mac,	"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
							net_params.guest_mac,
							net_params.guest_mac+1,
							net_params.guest_mac+2,
							net_params.guest_mac+3,
							net_params.guest_mac+4,
							net_params.guest_mac+5);

		virtio_net__init(&net_params);
	}

	kvm__start_timer(kvm);

	for (i = 0; i < nrcpus; i++) {
		kvm_cpus[i] = kvm_cpu__init(kvm, i);
		if (!kvm_cpus[i])
			die("unable to initialize KVM VCPU");

		if (single_step)
			kvm_cpu__enable_singlestep(kvm_cpus[i]);

		if (pthread_create(&kvm_cpus[i]->thread, NULL, kvm_cpu_thread, kvm_cpus[i]) != 0)
			die("unable to create KVM VCPU thread");
	}

	for (i = 0; i < nrcpus; i++) {
		void *ret;

		if (pthread_join(kvm_cpus[i]->thread, &ret) != 0)
			die("pthread_join");

		if (ret != NULL)
			exit_code	= 1;
	}

	disk_image__close(kvm->disk_image);
	kvm__delete(kvm);

	if (!exit_code)
		printf("\n  # KVM session ended normally.\n");

	return exit_code;
}
