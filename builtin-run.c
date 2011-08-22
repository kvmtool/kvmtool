#include "kvm/builtin-run.h"

#include "kvm/virtio-balloon.h"
#include "kvm/virtio-console.h"
#include "kvm/parse-options.h"
#include "kvm/8250-serial.h"
#include "kvm/framebuffer.h"
#include "kvm/disk-image.h"
#include "kvm/threadpool.h"
#include "kvm/virtio-blk.h"
#include "kvm/virtio-net.h"
#include "kvm/virtio-rng.h"
#include "kvm/ioeventfd.h"
#include "kvm/virtio-9p.h"
#include "kvm/barrier.h"
#include "kvm/kvm-cpu.h"
#include "kvm/ioport.h"
#include "kvm/symbol.h"
#include "kvm/i8042.h"
#include "kvm/mutex.h"
#include "kvm/term.h"
#include "kvm/util.h"
#include "kvm/vesa.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/rtc.h"
#include "kvm/sdl.h"
#include "kvm/vnc.h"
#include "kvm/guest_compat.h"

#include <linux/types.h>

#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#define DEFAULT_KVM_DEV		"/dev/kvm"
#define DEFAULT_CONSOLE		"serial"
#define DEFAULT_NETWORK		"user"
#define DEFAULT_HOST_ADDR	"192.168.33.1"
#define DEFAULT_GUEST_ADDR	"192.168.33.15"
#define DEFAULT_GUEST_MAC	"02:15:15:15:15:15"
#define DEFAULT_HOST_MAC	"02:01:01:01:01:01"
#define DEFAULT_SCRIPT		"none"

#define MB_SHIFT		(20)
#define MIN_RAM_SIZE_MB		(64ULL)
#define MIN_RAM_SIZE_BYTE	(MIN_RAM_SIZE_MB << MB_SHIFT)

struct kvm *kvm;
struct kvm_cpu *kvm_cpus[KVM_NR_CPUS];
__thread struct kvm_cpu *current_kvm_cpu;

static u64 ram_size;
static u8  image_count;
static bool virtio_rng;
static const char *kernel_cmdline;
static const char *kernel_filename;
static const char *vmlinux_filename;
static const char *initrd_filename;
static const char *image_filename[MAX_DISK_IMAGES];
static const char *console;
static const char *dev;
static const char *network;
static const char *host_ip;
static const char *guest_ip;
static const char *guest_mac;
static const char *host_mac;
static const char *script;
static const char *guest_name;
static bool single_step;
static bool readonly_image[MAX_DISK_IMAGES];
static bool vnc;
static bool sdl;
static bool balloon;
static bool using_rootfs;
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
	struct stat st;

	if (stat(arg, &st) == 0 &&
	    S_ISDIR(st.st_mode)) {
		char tmp[PATH_MAX];

		if (realpath(arg, tmp) == 0 ||
		    virtio_9p__register(kvm, tmp, "/dev/root") < 0)
			die("Unable to initialize virtio 9p");
		using_rootfs = 1;
		return 0;
	}

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
	if (realpath(arg, tmp)) {
		if (virtio_9p__register(kvm, tmp, tag_name) < 0)
			die("Unable to initialize virtio 9p");
	} else
		die("Failed resolving 9p path");
	return 0;
}


static const struct option options[] = {
	OPT_GROUP("Basic options:"),
	OPT_STRING('\0', "name", &guest_name, "guest name",
			"A name for the guest"),
	OPT_INTEGER('c', "cpus", &nrcpus, "Number of CPUs"),
	OPT_U64('m', "mem", &ram_size, "Virtual machine memory size in MiB."),
	OPT_CALLBACK('d', "disk", NULL, "image or rootfs_dir", "Disk image or rootfs directory", img_name_parser),
	OPT_BOOLEAN('\0', "balloon", &balloon, "Enable virtio balloon"),
	OPT_BOOLEAN('\0', "vnc", &vnc, "Enable VNC framebuffer"),
	OPT_BOOLEAN('\0', "sdl", &sdl, "Enable SDL framebuffer"),
	OPT_BOOLEAN('\0', "rng", &virtio_rng, "Enable virtio Random Number Generator"),
	OPT_CALLBACK('\0', "9p", NULL, "dir_to_share,tag_name",
		     "Enable virtio 9p to share files between host and guest", virtio_9p_rootdir_parser),
	OPT_STRING('\0', "console", &console, "serial or virtio",
			"Console to use"),
	OPT_STRING('\0', "dev", &dev, "device_file", "KVM device file"),

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
	OPT_STRING('\0', "host-ip", &host_ip, "a.b.c.d",
			"Assign this address to the host side networking"),
	OPT_STRING('\0', "guest-ip", &guest_ip, "a.b.c.d",
			"Assign this address to the guest side networking"),
	OPT_STRING('\0', "host-mac", &host_mac, "aa:bb:cc:dd:ee:ff",
			"Assign this address to the host side NIC"),
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
	OPT_BOOLEAN('\0', "debug-ioport", &ioport_debug,
			"Enable ioport debugging"),
	OPT_INTEGER('\0', "debug-iodelay", &debug_iodelay,
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
	if (sig == SIGKVMRESUME && is_paused)
		kvm__continue();
	else if (sig == SIGUSR2 && !is_paused)
		kvm__pause();
	else
		return;

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

static void handle_sigstop(int sig)
{
	kvm_cpu__reboot();
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

void kvm_run_help(void)
{
	usage_with_options(run_usage, options);
}

int kvm_cmd_run(int argc, const char **argv, const char *prefix)
{
	struct virtio_net_parameters net_params;
	static char real_cmdline[2048], default_name[20];
	struct framebuffer *fb = NULL;
	unsigned int nr_online_cpus;
	int exit_code = 0;
	int max_cpus, recommended_cpus;
	int i;
	void *ret;

	signal(SIGALRM, handle_sigalrm);
	signal(SIGQUIT, handle_sigquit);
	signal(SIGUSR1, handle_sigusr1);
	signal(SIGUSR2, handle_sigusr2);
	signal(SIGKVMSTOP, handle_sigstop);
	signal(SIGKVMRESUME, handle_sigusr2);
	/* ignore balloon signal by default if not enable balloon optiion */
	signal(SIGKVMADDMEM, SIG_IGN);
	signal(SIGKVMDELMEM, SIG_IGN);

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

	if (!dev)
		dev = DEFAULT_KVM_DEV;

	if (!console)
		console = DEFAULT_CONSOLE;

	if (!strncmp(console, "virtio", 6))
		active_console  = CONSOLE_VIRTIO;
	else
		active_console  = CONSOLE_8250;

	if (!host_ip)
		host_ip = DEFAULT_HOST_ADDR;

	if (!guest_ip)
		guest_ip = DEFAULT_GUEST_ADDR;

	if (!guest_mac)
		guest_mac = DEFAULT_GUEST_MAC;

	if (!host_mac)
		host_mac = DEFAULT_HOST_MAC;

	if (!script)
		script = DEFAULT_SCRIPT;

	symbol__init(vmlinux_filename);

	term_init();

	if (!guest_name) {
		sprintf(default_name, "guest-%u", getpid());
		guest_name = default_name;
	}

	kvm = kvm__init(dev, ram_size, guest_name);

	irq__init(kvm);

	kvm->single_step = single_step;

	ioeventfd__init();

	max_cpus = kvm__max_cpus(kvm);
	recommended_cpus = kvm__recommended_cpus(kvm);

	if (nrcpus > max_cpus) {
		printf("  # Limit the number of CPUs to %d\n", max_cpus);
		kvm->nrcpus	= max_cpus;
	} else if (nrcpus > recommended_cpus) {
		printf("  # Warning: The maximum recommended amount of VCPUs"
			" is %d\n", recommended_cpus);
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
	strcpy(real_cmdline, "notsc noapic noacpi pci=conf1 reboot=k panic=1 i8042.direct=1 i8042.dumbkbd=1 i8042.nopnp=1");
	if (vnc || sdl) {
		strcat(real_cmdline, " video=vesafb console=tty0");
	} else
		strcat(real_cmdline, " console=ttyS0 earlyprintk=serial");
	strcat(real_cmdline, " ");
	if (kernel_cmdline)
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));

	if (!using_rootfs && !image_filename[0]) {
		if (virtio_9p__register(kvm, "/", "/dev/root") < 0)
			die("Unable to initialize virtio 9p");

		using_rootfs = 1;

		if (!strstr(real_cmdline, "init="))
			strlcat(real_cmdline, " init=/bin/sh ", sizeof(real_cmdline));
	}

	if (!strstr(real_cmdline, "root="))
		strlcat(real_cmdline, " root=/dev/vda rw ", sizeof(real_cmdline));

	if (using_rootfs)
		strcat(real_cmdline, " root=/dev/root rootflags=rw,trans=virtio,version=9p2000.u rootfstype=9p");

	if (image_count) {
		kvm->nr_disks = image_count;
		kvm->disks    = disk_image__open_all(image_filename, readonly_image, image_count);
		if (!kvm->disks)
			die("Unable to load all disk images.");

		virtio_blk__init_all(kvm);
	}

	printf("  # kvm run -k %s -m %Lu -c %d --name %s\n", kernel_filename, ram_size / 1024 / 1024, nrcpus, guest_name);

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
		virtio_rng__init(kvm);

	if (balloon)
		virtio_bln__init(kvm);

	if (!network)
		network = DEFAULT_NETWORK;

	virtio_9p__init(kvm);

	if (strncmp(network, "none", 4)) {
		net_params.guest_ip = guest_ip;
		net_params.host_ip = host_ip;
		net_params.kvm = kvm;
		net_params.script = script;
		sscanf(guest_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			net_params.guest_mac,
			net_params.guest_mac+1,
			net_params.guest_mac+2,
			net_params.guest_mac+3,
			net_params.guest_mac+4,
			net_params.guest_mac+5);
		sscanf(host_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			net_params.host_mac,
			net_params.host_mac+1,
			net_params.host_mac+2,
			net_params.host_mac+3,
			net_params.host_mac+4,
			net_params.host_mac+5);

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
		if (kvm_cpus[i]->is_running) {
			pthread_kill(kvm_cpus[i]->thread, SIGKVMEXIT);
			if (pthread_join(kvm_cpus[i]->thread, &ret) != 0)
				die("pthread_join");
		}
		if (ret != NULL)
			exit_code = 1;
	}

	compat__print_all_messages();

	fb__stop();

	virtio_blk__delete_all(kvm);
	virtio_rng__delete_all(kvm);

	disk_image__close_all(kvm->disks, image_count);
	kvm__delete(kvm);

	if (!exit_code)
		printf("\n  # KVM session ended normally.\n");

	return exit_code;
}
