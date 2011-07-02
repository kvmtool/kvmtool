#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
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
#include <kvm/virtio-rng.h>
#include <kvm/disk-image.h>
#include <kvm/util.h>
#include <kvm/pci.h>
#include <kvm/rtc.h>
#include <kvm/term.h>
#include <kvm/ioport.h>
#include <kvm/threadpool.h>
#include <kvm/barrier.h>
#include <kvm/symbol.h>
#include <kvm/virtio-9p.h>
#include <kvm/vesa.h>
#include <kvm/ioeventfd.h>
#include <kvm/i8042.h>
#include <kvm/vnc.h>
#include <kvm/sdl.h>
#include <kvm/framebuffer.h>

/* header files for gitish interface  */
#include <kvm/kvm-run.h>
#include <kvm/parse-options.h>
#include <kvm/mutex.h>

#define DEFAULT_KVM_DEV		"/dev/kvm"
#define DEFAULT_CONSOLE		"serial"
#define DEFAULT_NETWORK		"user"
#define DEFAULT_HOST_ADDR	"192.168.33.1"
#define DEFAULT_GUEST_MAC	"00:15:15:15:15:15"
#define DEFAULT_SCRIPT		"none"

#define MB_SHIFT		(20)
#define MIN_RAM_SIZE_MB		(64ULL)
#define MIN_RAM_SIZE_BYTE	(MIN_RAM_SIZE_MB << MB_SHIFT)

struct kvm *kvm;
struct kvm_cpu *kvm_cpus[KVM_NR_CPUS];
__thread struct kvm_cpu *current_kvm_cpu;

static u64 ram_size;
static u8  image_count;
static int virtio_rng;
static const char *kernel_cmdline;
static const char *kernel_filename;
static const char *vmlinux_filename;
static const char *initrd_filename;
static const char *image_filename[MAX_DISK_IMAGES];
static const char *console;
static const char *kvm_dev;
static const char *network;
static const char *host_ip_addr;
static const char *guest_mac;
static const char *script;
static const char *guest_name;
static bool single_step;
static bool readonly_image[MAX_DISK_IMAGES];
static bool vnc;
static bool sdl;
extern bool ioport_debug;
extern int  active_console;
extern int  debug_iodelay;

bool do_debug_print = false;

static int nrcpus;
static int vidmode = -1;

static const char * const run_usage[] = {
	"kvm run [<options>] [<kernel image>]",
	NULL
};

static int img_name_parser(const struct option *opt, const char *arg, int unset)
{
	char *sep;

	if (image_count >= MAX_DISK_IMAGES)
		die("Currently only 4 images are supported");

	image_filename[image_count] = arg;
	sep = strstr(arg, ",");
	if (sep) {
		if (strcmp(sep + 1, "ro") == 0)
			readonly_image[image_count] = 1;
		*sep = 0;
	}

	image_count++;

	return 0;
}

static int virtio_9p_rootdir_parser(const struct option *opt, const char *arg, int unset)
{
	char *tag_name;
	char tmp[PATH_MAX];

	/*
	 * 9p dir can be of the form dirname,tag_name or
	 * just dirname. In the later case we use the
	 * default tag name
	 */
	tag_name = strstr(arg, ",");
	if (tag_name) {
		*tag_name = '\0';
		tag_name++;
	}
	if (realpath(arg, tmp))
		virtio_9p__init(kvm, tmp, tag_name);
	else
		die("Failed resolving 9p path");
	return 0;
}


static const struct option options[] = {
	OPT_GROUP("Basic options:"),
	OPT_STRING('\0', "name", &guest_name, "guest name",
			"A name for the guest"),
	OPT_INTEGER('c', "cpus", &nrcpus, "Number of CPUs"),
	OPT_U64('m', "mem", &ram_size, "Virtual machine memory size in MiB."),
	OPT_CALLBACK('d', "disk", NULL, "image", "Disk image", img_name_parser),
	OPT_STRING('\0', "console", &console, "serial or virtio",
			"Console to use"),
	OPT_INCR('\0', "rng", &virtio_rng,
			"Enable virtio Random Number Generator"),
	OPT_STRING('\0', "kvm-dev", &kvm_dev, "kvm-dev", "KVM device file"),
	OPT_CALLBACK('\0', "virtio-9p", NULL, "dirname,tag_name",
		     "Enable 9p over virtio", virtio_9p_rootdir_parser),
	OPT_BOOLEAN('\0', "vnc", &vnc, "Enable VNC framebuffer"),
	OPT_BOOLEAN('\0', "sdl", &sdl, "Enable SDL framebuffer"),

	OPT_GROUP("Kernel options:"),
	OPT_STRING('k', "kernel", &kernel_filename, "kernel",
			"Kernel to boot in virtual machine"),
	OPT_STRING('i', "initrd", &initrd_filename, "initrd",
			"Initial RAM disk image"),
	OPT_STRING('p', "params", &kernel_cmdline, "params",
			"Kernel command line arguments"),

	OPT_GROUP("Networking options:"),
	OPT_STRING('n', "network", &network, "user, tap, none",
			"Network to use"),
	OPT_STRING('\0', "host-ip-addr", &host_ip_addr, "a.b.c.d",
			"Assign this address to the host side networking"),
	OPT_STRING('\0', "guest-mac", &guest_mac, "aa:bb:cc:dd:ee:ff",
			"Assign this address to the guest side NIC"),
	OPT_STRING('\0', "tapscript", &script, "Script path",
			 "Assign a script to process created tap device"),

	OPT_GROUP("BIOS options:"),
	OPT_INTEGER('\0', "vidmode", &vidmode,
		    "Video mode"),

	OPT_GROUP("Debug options:"),
	OPT_BOOLEAN('\0', "debug", &do_debug_print,
			"Enable debug messages"),
	OPT_BOOLEAN('\0', "debug-single-step", &single_step,
			"Enable single stepping"),
	OPT_BOOLEAN('\0', "debug-ioport-debug", &ioport_debug,
			"Enable ioport debugging"),
	OPT_INTEGER('\0', "debug_iodelay", &debug_iodelay,
			"Delay IO by millisecond"),
	OPT_END()
};

/*
 * Serialize debug printout so that the output of multiple vcpus does not
 * get mixed up:
 */
static int printout_done;

static void handle_sigusr1(int sig)
{
	struct kvm_cpu *cpu = current_kvm_cpu;

	if (!cpu)
		return;

	printf("\n #\n # vCPU #%ld's dump:\n #\n", cpu->cpu_id);
	kvm_cpu__show_registers(cpu);
	kvm_cpu__show_code(cpu);
	kvm_cpu__show_page_tables(cpu);
	fflush(stdout);
	printout_done = 1;
	mb();
}

/* Pause/resume the guest using SIGUSR2 */
static int is_paused;

static void handle_sigusr2(int sig)
{
	if (is_paused)
		kvm__continue();
	else
		kvm__pause();

	is_paused = !is_paused;
	pr_info("Guest %s\n", is_paused ? "paused" : "resumed");
}

static void handle_sigquit(int sig)
{
	int i;

	for (i = 0; i < nrcpus; i++) {
		struct kvm_cpu *cpu = kvm_cpus[i];

		if (!cpu)
			continue;

		printout_done = 0;
		pthread_kill(cpu->thread, SIGUSR1);
		/*
		 * Wait for the vCPU to dump state before signalling
		 * the next thread. Since this is debug code it does
		 * not matter that we are burning CPU time a bit:
		 */
		while (!printout_done)
			mb();
	}

	serial8250__inject_sysrq(kvm);
}

static void handle_sigalrm(int sig)
{
	serial8250__inject_interrupt(kvm);
	virtio_console__inject_interrupt(kvm);
}

static void *kvm_cpu_thread(void *arg)
{
	current_kvm_cpu		= arg;

	if (kvm_cpu__start(current_kvm_cpu))
		goto panic_kvm;

	kvm_cpu__delete(current_kvm_cpu);

	return (void *) (intptr_t) 0;

panic_kvm:
	fprintf(stderr, "KVM exit reason: %u (\"%s\")\n",
		current_kvm_cpu->kvm_run->exit_reason,
		kvm_exit_reasons[current_kvm_cpu->kvm_run->exit_reason]);
	if (current_kvm_cpu->kvm_run->exit_reason == KVM_EXIT_UNKNOWN)
		fprintf(stderr, "KVM exit code: 0x%Lu\n",
			current_kvm_cpu->kvm_run->hw.hardware_exit_reason);

	kvm_cpu__show_registers(current_kvm_cpu);
	kvm_cpu__show_code(current_kvm_cpu);
	kvm_cpu__show_page_tables(current_kvm_cpu);

	kvm_cpu__delete(current_kvm_cpu);

	return (void *) (intptr_t) 1;
}

static char kernel[PATH_MAX];

static const char *host_kernels[] = {
	"/boot/vmlinuz",
	"/boot/bzImage",
	NULL
};

static const char *default_kernels[] = {
	"./bzImage",
	"../../arch/x86/boot/bzImage",
	NULL
};

static const char *default_vmlinux[] = {
	"../../../vmlinux",
	"../../vmlinux",
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
	fprintf(stderr, "\nPlease see 'kvm run --help' for more options.\n\n");
}

static u64 host_ram_size(void)
{
	long page_size;
	long nr_pages;

	nr_pages	= sysconf(_SC_PHYS_PAGES);
	if (nr_pages < 0) {
		pr_warning("sysconf(_SC_PHYS_PAGES) failed");
		return 0;
	}

	page_size	= sysconf(_SC_PAGE_SIZE);
	if (page_size < 0) {
		pr_warning("sysconf(_SC_PAGE_SIZE) failed");
		return 0;
	}

	return (nr_pages * page_size) >> MB_SHIFT;
}

/*
 * If user didn't specify how much memory it wants to allocate for the guest,
 * avoid filling the whole host RAM.
 */
#define RAM_SIZE_RATIO		0.8

static u64 get_ram_size(int nr_cpus)
{
	u64 available;
	u64 ram_size;

	ram_size	= 64 * (nr_cpus + 3);

	available	= host_ram_size() * RAM_SIZE_RATIO;
	if (!available)
		available = MIN_RAM_SIZE_MB;

	if (ram_size > available)
		ram_size	= available;

	return ram_size;
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

static const char *find_vmlinux(void)
{
	const char **vmlinux;

	vmlinux = &default_vmlinux[0];
	while (*vmlinux) {
		struct stat st;

		if (stat(*vmlinux, &st) < 0 || !S_ISREG(st.st_mode)) {
			vmlinux++;
			continue;
		}
		return *vmlinux;
	}
	return NULL;
}

static int root_device(char *dev, long *part)
{
	struct stat st;

	if (stat("/", &st) < 0)
		return -1;

	*part = minor(st.st_dev);

	sprintf(dev, "/dev/block/%u:0", major(st.st_dev));
	if (access(dev, R_OK) < 0)
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

void kvm_run_help(void)
{
	usage_with_options(run_usage, options);
}

int kvm_cmd_run(int argc, const char **argv, const char *prefix)
{
	struct virtio_net_parameters net_params;
	static char real_cmdline[2048];
	struct framebuffer *fb = NULL;
	unsigned int nr_online_cpus;
	int exit_code = 0;
	int max_cpus;
	char *hi;
	int i;
	void *ret;

	signal(SIGALRM, handle_sigalrm);
	signal(SIGQUIT, handle_sigquit);
	signal(SIGUSR1, handle_sigusr1);
	signal(SIGUSR2, handle_sigusr2);

	nr_online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

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

	vmlinux_filename = find_vmlinux();

	if (nrcpus == 0)
		nrcpus = nr_online_cpus;
	else if (nrcpus < 1 || nrcpus > KVM_NR_CPUS)
		die("Number of CPUs %d is out of [1;%d] range", nrcpus, KVM_NR_CPUS);

	if (!ram_size)
		ram_size	= get_ram_size(nrcpus);

	if (ram_size < MIN_RAM_SIZE_MB)
		die("Not enough memory specified: %lluMB (min %lluMB)", ram_size, MIN_RAM_SIZE_MB);

	if (ram_size > host_ram_size())
		pr_warning("Guest memory size %lluMB exceeds host physical RAM size %lluMB", ram_size, host_ram_size());

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

	symbol__init(vmlinux_filename);

	term_init();

	kvm = kvm__init(kvm_dev, ram_size, guest_name);

	ioeventfd__init();

	max_cpus = kvm__max_cpus(kvm);

	if (nrcpus > max_cpus) {
		printf("  # Limit the number of CPUs to %d\n", max_cpus);
		kvm->nrcpus	= max_cpus;
	}

	kvm->nrcpus = nrcpus;

	/*
	 * vidmode should be either specified
	 * either set by default
	 */
	if (vnc || sdl) {
		if (vidmode == -1)
			vidmode = 0x312;
	} else
		vidmode = 0;

	memset(real_cmdline, 0, sizeof(real_cmdline));
	strcpy(real_cmdline, "notsc noapic noacpi pci=conf1 reboot=k panic=1");
	if (vnc || sdl) {
		strcat(real_cmdline, " video=vesafb console=tty0");
	} else
		strcat(real_cmdline, " console=ttyS0 earlyprintk=serial");
	strcat(real_cmdline, " ");
	if (kernel_cmdline)
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));

	hi = NULL;
	if (!image_filename[0]) {
		hi = host_image(real_cmdline, sizeof(real_cmdline));
		if (hi) {
			image_filename[0] = hi;
			readonly_image[0] = true;
			image_count++;
		}
	}

	if (!strstr(real_cmdline, "root="))
		strlcat(real_cmdline, " root=/dev/vda rw ", sizeof(real_cmdline));

	if (image_count) {
		kvm->nr_disks = image_count;
		kvm->disks    = disk_image__open_all(image_filename, readonly_image, image_count);
		if (!kvm->disks)
			die("Unable to load all disk images.");

		virtio_blk__init_all(kvm);
	}

	free(hi);

	printf("  # kvm run -k %s -m %Lu -c %d\n", kernel_filename, ram_size / 1024 / 1024, nrcpus);

	if (!kvm__load_kernel(kvm, kernel_filename, initrd_filename,
				real_cmdline, vidmode))
		die("unable to load kernel %s", kernel_filename);

	kvm->vmlinux		= vmlinux_filename;

	ioport__setup_legacy();

	rtc__init();

	serial8250__init(kvm);

	pci__init();

	if (active_console == CONSOLE_VIRTIO)
		virtio_console__init(kvm);

	if (virtio_rng)
		while (virtio_rng--)
			virtio_rng__init(kvm);

	if (!network)
		network = DEFAULT_NETWORK;

	if (strncmp(network, "none", 4)) {
		net_params.host_ip = host_ip_addr;
		net_params.kvm = kvm;
		net_params.script = script;
		sscanf(guest_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			net_params.guest_mac,
			net_params.guest_mac+1,
			net_params.guest_mac+2,
			net_params.guest_mac+3,
			net_params.guest_mac+4,
			net_params.guest_mac+5);

		if (!strncmp(network, "user", 4))
			net_params.mode = NET_MODE_USER;
		else if (!strncmp(network, "tap", 3))
			net_params.mode = NET_MODE_TAP;
		else
			die("Unkown network mode %s, please use -network user, tap, none", network);
		virtio_net__init(&net_params);
	}

	kvm__start_timer(kvm);

	kvm__setup_bios(kvm);

	for (i = 0; i < nrcpus; i++) {
		kvm_cpus[i] = kvm_cpu__init(kvm, i);
		if (!kvm_cpus[i])
			die("unable to initialize KVM VCPU");

		if (single_step)
			kvm_cpu__enable_singlestep(kvm_cpus[i]);
	}

	kvm__init_ram(kvm);

	kbd__init(kvm);

	if (vnc || sdl)
		fb = vesa__init(kvm);

	if (vnc) {
		if (fb)
			vnc__init(fb);
	}

	if (sdl) {
		if (fb)
			sdl__init(fb);
	}

	fb__start();

	thread_pool__init(nr_online_cpus);
	ioeventfd__start();

	for (i = 0; i < nrcpus; i++) {
		if (pthread_create(&kvm_cpus[i]->thread, NULL, kvm_cpu_thread, kvm_cpus[i]) != 0)
			die("unable to create KVM VCPU thread");
	}

	/* Only VCPU #0 is going to exit by itself when shutting down */
	if (pthread_join(kvm_cpus[0]->thread, &ret) != 0)
		exit_code = 1;

	for (i = 1; i < nrcpus; i++) {
		pthread_kill(kvm_cpus[i]->thread, SIGKVMEXIT);
		if (pthread_join(kvm_cpus[i]->thread, &ret) != 0)
			die("pthread_join");

		if (ret != NULL)
			exit_code = 1;
	}

	fb__stop();

	virtio_blk__delete_all(kvm);
	virtio_rng__delete_all(kvm);

	disk_image__close_all(kvm->disks, image_count);
	kvm__delete(kvm);

	if (!exit_code)
		printf("\n  # KVM session ended normally.\n");

	return exit_code;
}
