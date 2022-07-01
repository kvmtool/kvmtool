#include "kvm/builtin-run.h"

#include "kvm/builtin-setup.h"
#include "kvm/virtio-balloon.h"
#include "kvm/virtio-console.h"
#include "kvm/parse-options.h"
#include "kvm/8250-serial.h"
#include "kvm/framebuffer.h"
#include "kvm/disk-image.h"
#include "kvm/threadpool.h"
#include "kvm/virtio-scsi.h"
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
#include "kvm/strbuf.h"
#include "kvm/vesa.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/rtc.h"
#include "kvm/sdl.h"
#include "kvm/vnc.h"
#include "kvm/guest_compat.h"
#include "kvm/kvm-ipc.h"
#include "kvm/builtin-debug.h"

#include <linux/types.h>
#include <linux/err.h>
#include <linux/sizes.h>

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

#define KB_SHIFT		(10)
#define MB_SHIFT		(20)
#define GB_SHIFT		(30)
#define TB_SHIFT		(40)
#define PB_SHIFT		(50)

__thread struct kvm_cpu *current_kvm_cpu;

static int  kvm_run_wrapper;

bool do_debug_print = false;

static const char * const run_usage[] = {
	"lkvm run [<options>] [<kernel image>]",
	NULL
};

enum {
	KVM_RUN_DEFAULT,
	KVM_RUN_SANDBOX,
};

static int img_name_parser(const struct option *opt, const char *arg, int unset)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, PATH_MAX, "%s%s", kvm__get_dir(), arg);

	if ((stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) ||
	   (stat(path, &st) == 0 && S_ISDIR(st.st_mode)))
		return virtio_9p_img_name_parser(opt, arg, unset);
	return disk_img_name_parser(opt, arg, unset);
}

void kvm_run_set_wrapper_sandbox(void)
{
	kvm_run_wrapper = KVM_RUN_SANDBOX;
}

static int parse_mem_unit(char **next)
{
	switch (**next) {
	case 'B': case 'b': (*next)++; return 0;
	case 'K': case 'k': (*next)++; return KB_SHIFT;
	case 'M': case 'm': (*next)++; return MB_SHIFT;
	case 'G': case 'g': (*next)++; return GB_SHIFT;
	case 'T': case 't': (*next)++; return TB_SHIFT;
	case 'P': case 'p': (*next)++; return PB_SHIFT;
	}

	return MB_SHIFT;
}

static u64 parse_mem_option(const char *nptr, char **next)
{
	u64 shift;
	u64 val;

	errno = 0;
	val = strtoull(nptr, next, 10);
	if (errno == ERANGE)
		die("Memory too large: %s", nptr);
	if (*next == nptr)
		die("Invalid memory specifier: %s", nptr);

	shift = parse_mem_unit(next);
	if ((val << shift) < val)
		die("Memory too large: %s", nptr);

	return val << shift;
}

static int mem_parser(const struct option *opt, const char *arg, int unset)
{
	struct kvm *kvm = opt->ptr;
	char *next, *nptr;

	kvm->cfg.ram_size = parse_mem_option(arg, &next);
	if (kvm->cfg.ram_size == 0)
		die("Invalid RAM size: %s", arg);

	if (kvm__arch_has_cfg_ram_address() && *next == '@') {
		next++;
		if (*next == '\0')
			die("Missing memory address: %s", arg);

		nptr = next;
		kvm->cfg.ram_addr = parse_mem_option(nptr, &next);
	}

	if (*next != '\0')
		die("Invalid memory specifier: %s", arg);

	return 0;
}

#ifndef OPT_ARCH_RUN
#define OPT_ARCH_RUN(...)
#endif

#ifdef ARCH_HAS_CFG_RAM_ADDRESS
#define MEM_OPT_HELP_SHORT	"size[BKMGTP][@addr[BKMGTP]]"
#define MEM_OPT_HELP_LONG						\
	"Virtual machine memory size and optional base address, both"	\
	" measured by default in megabytes (M)"
#else
#define MEM_OPT_HELP_SHORT	"size[BKMGTP]"
#define MEM_OPT_HELP_LONG						\
	"Virtual machine memory size, by default measured in"		\
	" in megabytes (M)"
#endif

#define BUILD_OPTIONS(name, cfg, kvm)					\
	struct option name[] = {					\
	OPT_GROUP("Basic options:"),					\
	OPT_STRING('\0', "name", &(cfg)->guest_name, "guest name",	\
			"A name for the guest"),			\
	OPT_INTEGER('c', "cpus", &(cfg)->nrcpus, "Number of CPUs"),	\
	OPT_CALLBACK('m', "mem", NULL, MEM_OPT_HELP_SHORT,		\
		     MEM_OPT_HELP_LONG, mem_parser, kvm),		\
	OPT_CALLBACK('d', "disk", kvm, "image or rootfs_dir", "Disk "	\
			" image or rootfs directory", img_name_parser,	\
			kvm),						\
	OPT_BOOLEAN('\0', "balloon", &(cfg)->balloon, "Enable virtio"	\
			" balloon"),					\
	OPT_BOOLEAN('\0', "vnc", &(cfg)->vnc, "Enable VNC framebuffer"),\
	OPT_BOOLEAN('\0', "gtk", &(cfg)->gtk, "Enable GTK framebuffer"),\
	OPT_BOOLEAN('\0', "sdl", &(cfg)->sdl, "Enable SDL framebuffer"),\
	OPT_BOOLEAN('\0', "rng", &(cfg)->virtio_rng, "Enable virtio"	\
			" Random Number Generator"),			\
	OPT_BOOLEAN('\0', "nodefaults", &(cfg)->nodefaults, "Disable"   \
			" implicit configuration that cannot be"	\
			" disabled otherwise"),				\
	OPT_CALLBACK('\0', "9p", NULL, "dir_to_share,tag_name",		\
		     "Enable virtio 9p to share files between host and"	\
		     " guest", virtio_9p_rootdir_parser, kvm),		\
	OPT_STRING('\0', "console", &(cfg)->console, "serial, virtio or"\
			" hv", "Console to use"),			\
	OPT_U64('\0', "vsock", &(cfg)->vsock_cid,			\
			"Guest virtio socket CID"),			\
	OPT_STRING('\0', "dev", &(cfg)->dev, "device_file",		\
			"KVM device file"),				\
	OPT_CALLBACK('\0', "tty", NULL, "tty id",			\
		     "Remap guest TTY into a pty on the host",		\
		     tty_parser, NULL),					\
	OPT_STRING('\0', "sandbox", &(cfg)->sandbox, "script",		\
			"Run this script when booting into custom"	\
			" rootfs"),					\
	OPT_STRING('\0', "hugetlbfs", &(cfg)->hugetlbfs_path, "path",	\
			"Hugetlbfs path"),				\
	OPT_BOOLEAN('\0', "virtio-legacy", &(cfg)->virtio_legacy,	\
		    "Use legacy virtio transport"),			\
									\
	OPT_GROUP("Kernel options:"),					\
	OPT_STRING('k', "kernel", &(cfg)->kernel_filename, "kernel",	\
			"Kernel to boot in virtual machine"),		\
	OPT_STRING('i', "initrd", &(cfg)->initrd_filename, "initrd",	\
			"Initial RAM disk image"),			\
	OPT_STRING('p', "params", &(cfg)->kernel_cmdline, "params",	\
			"Kernel command line arguments"),		\
	OPT_STRING('f', "firmware", &(cfg)->firmware_filename, "firmware",\
			"Firmware image to boot in virtual machine"),	\
	OPT_STRING('F', "flash", &(cfg)->flash_filename, "flash",\
			"Flash image to present to virtual machine"),	\
									\
	OPT_GROUP("Networking options:"),				\
	OPT_CALLBACK_DEFAULT('n', "network", NULL, "network params",	\
		     "Create a new guest NIC",				\
		     netdev_parser, NULL, kvm),				\
	OPT_BOOLEAN('\0', "no-dhcp", &(cfg)->no_dhcp, "Disable kernel"	\
			" DHCP in rootfs mode"),			\
									\
	OPT_GROUP("VFIO options:"),					\
	OPT_CALLBACK('\0', "vfio-pci", NULL, "[domain:]bus:dev.fn",	\
		     "Assign a PCI device to the virtual machine",	\
		     vfio_device_parser, kvm),				\
									\
	OPT_GROUP("Debug options:"),					\
	OPT_BOOLEAN('\0', "debug", &do_debug_print,			\
			"Enable debug messages"),			\
	OPT_BOOLEAN('\0', "debug-single-step", &(cfg)->single_step,	\
			"Enable single stepping"),			\
	OPT_BOOLEAN('\0', "debug-ioport", &(cfg)->ioport_debug,		\
			"Enable ioport debugging"),			\
	OPT_BOOLEAN('\0', "debug-mmio", &(cfg)->mmio_debug,		\
			"Enable MMIO debugging"),			\
	OPT_INTEGER('\0', "debug-iodelay", &(cfg)->debug_iodelay,	\
			"Delay IO by millisecond"),			\
									\
	OPT_ARCH(RUN, cfg)						\
	OPT_END()							\
	};

static void *kvm_cpu_thread(void *arg)
{
	char name[16];

	current_kvm_cpu = arg;

	sprintf(name, "kvm-vcpu-%lu", current_kvm_cpu->cpu_id);
	kvm__set_thread_name(name);

	if (kvm_cpu__start(current_kvm_cpu))
		goto panic_kvm;

	return (void *) (intptr_t) 0;

panic_kvm:
	fprintf(stderr, "KVM exit reason: %u (\"%s\")\n",
		current_kvm_cpu->kvm_run->exit_reason,
		kvm_exit_reasons[current_kvm_cpu->kvm_run->exit_reason]);
	if (current_kvm_cpu->kvm_run->exit_reason == KVM_EXIT_UNKNOWN)
		fprintf(stderr, "KVM exit code: 0x%llu\n",
			(unsigned long long)current_kvm_cpu->kvm_run->hw.hardware_exit_reason);

	kvm_cpu__set_debug_fd(STDOUT_FILENO);
	kvm_cpu__show_registers(current_kvm_cpu);
	kvm_cpu__show_code(current_kvm_cpu);
	kvm_cpu__show_page_tables(current_kvm_cpu);

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
	"arch/" BUILD_ARCH "/boot/bzImage",
	"../../arch/" BUILD_ARCH "/boot/bzImage",
	NULL
};

static const char *default_vmlinux[] = {
	"vmlinux",
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
	fprintf(stderr, "\nPlease see '%s run --help' for more options.\n\n",
		KVM_BINARY_NAME);
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

	return (u64)nr_pages * page_size;
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

	ram_size	= (u64)SZ_64M * (nr_cpus + 3);

	available	= host_ram_size() * RAM_SIZE_RATIO;
	if (!available)
		available = MIN_RAM_SIZE;

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
		strlcpy(kernel, *k, PATH_MAX);
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
	struct kvm *kvm = NULL;

	BUILD_OPTIONS(options, &kvm->cfg, kvm);
	usage_with_options(run_usage, options);
}

static int kvm_run_set_sandbox(struct kvm *kvm)
{
	const char *guestfs_name = kvm->cfg.custom_rootfs_name;
	char path[PATH_MAX], script[PATH_MAX], *tmp;

	snprintf(path, PATH_MAX, "%s%s/virt/sandbox.sh", kvm__get_dir(), guestfs_name);

	remove(path);

	if (kvm->cfg.sandbox == NULL)
		return 0;

	tmp = realpath(kvm->cfg.sandbox, NULL);
	if (tmp == NULL)
		return -ENOMEM;

	snprintf(script, PATH_MAX, "/host/%s", tmp);
	free(tmp);

	return symlink(script, path);
}

static void kvm_write_sandbox_cmd_exactly(int fd, const char *arg)
{
	const char *single_quote;

	if (!*arg) { /* zero length string */
		if (write(fd, "''", 2) <= 0)
			die("Failed writing sandbox script");
		return;
	}

	while (*arg) {
		single_quote = strchrnul(arg, '\'');

		/* write non-single-quote string as #('string') */
		if (arg != single_quote) {
			if (write(fd, "'", 1) <= 0 ||
			    write(fd, arg, single_quote - arg) <= 0 ||
			    write(fd, "'", 1) <= 0)
				die("Failed writing sandbox script");
		}

		/* write single quote as #("'") */
		if (*single_quote) {
			if (write(fd, "\"'\"", 3) <= 0)
				die("Failed writing sandbox script");
		} else
			break;

		arg = single_quote + 1;
	}
}

static void resolve_program(const char *src, char *dst, size_t len)
{
	struct stat st;
	int err;

	err = stat(src, &st);

	if (!err && S_ISREG(st.st_mode)) {
		char resolved_path[PATH_MAX];

		if (!realpath(src, resolved_path))
			die("Unable to resolve program %s: %s\n", src, strerror(errno));

		if (snprintf(dst, len, "/host%s", resolved_path) >= (int)len)
			die("Pathname too long: %s -> %s\n", src, resolved_path);

	} else
		strlcpy(dst, src, len);
}

static void kvm_run_write_sandbox_cmd(struct kvm *kvm, const char **argv, int argc)
{
	const char script_hdr[] = "#! /bin/bash\n\n";
	char program[PATH_MAX];
	int fd;

	remove(kvm->cfg.sandbox);

	fd = open(kvm->cfg.sandbox, O_RDWR | O_CREAT, 0777);
	if (fd < 0)
		die("Failed creating sandbox script");

	if (write(fd, script_hdr, sizeof(script_hdr) - 1) <= 0)
		die("Failed writing sandbox script");

	resolve_program(argv[0], program, PATH_MAX);
	kvm_write_sandbox_cmd_exactly(fd, program);

	argv++;
	argc--;

	while (argc) {
		if (write(fd, " ", 1) <= 0)
			die("Failed writing sandbox script");

		kvm_write_sandbox_cmd_exactly(fd, argv[0]);
		argv++;
		argc--;
	}
	if (write(fd, "\n", 1) <= 0)
		die("Failed writing sandbox script");

	close(fd);
}

static void kvm_run_set_real_cmdline(struct kvm *kvm)
{
	static char real_cmdline[2048];
	bool video;

	video = kvm->cfg.vnc || kvm->cfg.sdl || kvm->cfg.gtk;

	memset(real_cmdline, 0, sizeof(real_cmdline));
	kvm__arch_set_cmdline(real_cmdline, video);

	if (video) {
		strcat(real_cmdline, " console=tty0");
	} else {
		switch (kvm->cfg.active_console) {
		case CONSOLE_HV:
			/* Fallthrough */
		case CONSOLE_VIRTIO:
			strcat(real_cmdline, " console=hvc0");
			break;
		case CONSOLE_8250:
			strcat(real_cmdline, " console=ttyS0");
			break;
		}
	}

	if (kvm->cfg.using_rootfs) {
		strcat(real_cmdline, " rw rootflags=trans=virtio,version=9p2000.L,cache=loose rootfstype=9p");
		if (kvm->cfg.custom_rootfs) {
#ifdef CONFIG_GUEST_PRE_INIT
			strcat(real_cmdline, " init=/virt/pre_init");
#else
			strcat(real_cmdline, " init=/virt/init");
#endif
			if (!kvm->cfg.no_dhcp)
				strcat(real_cmdline, "  ip=dhcp");
		}
	} else if (!kvm->cfg.kernel_cmdline || !strstr(kvm->cfg.kernel_cmdline, "root=")) {
		strlcat(real_cmdline, " root=/dev/vda rw ", sizeof(real_cmdline));
	}

	if (kvm->cfg.kernel_cmdline) {
		strcat(real_cmdline, " ");
		strlcat(real_cmdline, kvm->cfg.kernel_cmdline, sizeof(real_cmdline));
	}

	kvm->cfg.real_cmdline = real_cmdline;
}

static void kvm_run_validate_cfg(struct kvm *kvm)
{
	u64 available_ram;

	if (kvm->cfg.kernel_filename && kvm->cfg.firmware_filename)
		die("Only one of --kernel or --firmware can be specified");

	if ((kvm->cfg.vnc && (kvm->cfg.sdl || kvm->cfg.gtk)) ||
	    (kvm->cfg.sdl && kvm->cfg.gtk))
		die("Only one of --vnc, --sdl or --gtk can be specified");

	if (kvm->cfg.firmware_filename && kvm->cfg.initrd_filename)
		pr_warning("Ignoring initrd file when loading a firmware image");

	if (kvm->cfg.ram_size) {
		available_ram = host_ram_size();
		if (available_ram && kvm->cfg.ram_size > available_ram) {
			pr_warning("Guest memory size %lluMB exceeds host physical RAM size %lluMB",
				(unsigned long long)kvm->cfg.ram_size >> MB_SHIFT,
				(unsigned long long)available_ram >> MB_SHIFT);
		}
	}

	kvm__arch_validate_cfg(kvm);
}

static struct kvm *kvm_cmd_run_init(int argc, const char **argv)
{
	static char default_name[20];
	unsigned int nr_online_cpus;
	struct kvm *kvm = kvm__new();

	if (IS_ERR(kvm))
		return kvm;

	nr_online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	kvm->cfg.custom_rootfs_name = "default";
	/*
	 * An architecture can allow the user to set the RAM base address to
	 * zero. Initialize the address before parsing the command line
	 * arguments, otherwise it will be impossible to distinguish between the
	 * user setting the base address to zero or letting it unset and using
	 * the default value.
	 */
	kvm->cfg.ram_addr = kvm__arch_default_ram_address();

	while (argc != 0) {
		BUILD_OPTIONS(options, &kvm->cfg, kvm);
		argc = parse_options(argc, argv, options, run_usage,
				PARSE_OPT_STOP_AT_NON_OPTION |
				PARSE_OPT_KEEP_DASHDASH);
		if (argc != 0) {
			/* Cusrom options, should have been handled elsewhere */
			if (strcmp(argv[0], "--") == 0) {
				if (kvm_run_wrapper == KVM_RUN_SANDBOX) {
					kvm->cfg.sandbox = DEFAULT_SANDBOX_FILENAME;
					kvm_run_write_sandbox_cmd(kvm, argv+1, argc-1);
					break;
				}
			}

			if ((kvm_run_wrapper == KVM_RUN_DEFAULT && kvm->cfg.kernel_filename) ||
				(kvm_run_wrapper == KVM_RUN_SANDBOX && kvm->cfg.sandbox)) {
				fprintf(stderr, "Cannot handle parameter: "
						"%s\n", argv[0]);
				usage_with_options(run_usage, options);
				free(kvm);
				return ERR_PTR(-EINVAL);
			}
			if (kvm_run_wrapper == KVM_RUN_SANDBOX) {
				/*
				 * first unhandled parameter is treated as
				 * sandbox command
				 */
				kvm->cfg.sandbox = DEFAULT_SANDBOX_FILENAME;
				kvm_run_write_sandbox_cmd(kvm, argv, argc);
			} else {
				/*
				 * first unhandled parameter is treated as a kernel
				 * image
				 */
				kvm->cfg.kernel_filename = argv[0];
			}
			argv++;
			argc--;
		}

	}

	kvm_run_validate_cfg(kvm);

	if (!kvm->cfg.kernel_filename && !kvm->cfg.firmware_filename) {
		kvm->cfg.kernel_filename = find_kernel();

		if (!kvm->cfg.kernel_filename) {
			kernel_usage_with_options();
			return ERR_PTR(-EINVAL);
		}
	}

	if (kvm->cfg.kernel_filename) {
		kvm->cfg.vmlinux_filename = find_vmlinux();
		kvm->vmlinux = kvm->cfg.vmlinux_filename;
	}

	if (kvm->cfg.nrcpus == 0)
		kvm->cfg.nrcpus = nr_online_cpus;

	if (!kvm->cfg.ram_size)
		kvm->cfg.ram_size = get_ram_size(kvm->cfg.nrcpus);

	if (!kvm->cfg.dev)
		kvm->cfg.dev = DEFAULT_KVM_DEV;

	if (!kvm->cfg.console)
		kvm->cfg.console = DEFAULT_CONSOLE;

	if (!strncmp(kvm->cfg.console, "virtio", 6))
		kvm->cfg.active_console  = CONSOLE_VIRTIO;
	else if (!strncmp(kvm->cfg.console, "serial", 6))
		kvm->cfg.active_console  = CONSOLE_8250;
	else if (!strncmp(kvm->cfg.console, "hv", 2))
		kvm->cfg.active_console = CONSOLE_HV;
	else
		pr_warning("No console!");

	if (!kvm->cfg.host_ip)
		kvm->cfg.host_ip = DEFAULT_HOST_ADDR;

	if (!kvm->cfg.guest_ip)
		kvm->cfg.guest_ip = DEFAULT_GUEST_ADDR;

	if (!kvm->cfg.guest_mac)
		kvm->cfg.guest_mac = DEFAULT_GUEST_MAC;

	if (!kvm->cfg.host_mac)
		kvm->cfg.host_mac = DEFAULT_HOST_MAC;

	if (!kvm->cfg.script)
		kvm->cfg.script = DEFAULT_SCRIPT;

	if (!kvm->cfg.network)
                kvm->cfg.network = DEFAULT_NETWORK;

	if (!kvm->cfg.guest_name) {
		if (kvm->cfg.custom_rootfs) {
			kvm->cfg.guest_name = kvm->cfg.custom_rootfs_name;
		} else {
			sprintf(default_name, "guest-%u", getpid());
			kvm->cfg.guest_name = default_name;
		}
	}

	if (!kvm->cfg.nodefaults &&
	    !kvm->cfg.using_rootfs &&
	    !kvm->cfg.disk_image[0].filename &&
	    !kvm->cfg.initrd_filename) {
		char tmp[PATH_MAX];

		kvm_setup_create_new(kvm->cfg.custom_rootfs_name);
		kvm_setup_resolv(kvm->cfg.custom_rootfs_name);

		snprintf(tmp, PATH_MAX, "%s%s", kvm__get_dir(), "default");
		if (virtio_9p__register(kvm, tmp, "/dev/root") < 0)
			die("Unable to initialize virtio 9p");
		if (virtio_9p__register(kvm, "/", "hostfs") < 0)
			die("Unable to initialize virtio 9p");
		kvm->cfg.using_rootfs = kvm->cfg.custom_rootfs = 1;
	}

	if (kvm->cfg.custom_rootfs) {
		kvm_run_set_sandbox(kvm);
		if (kvm_setup_guest_init(kvm->cfg.custom_rootfs_name))
			die("Failed to setup init for guest.");
	}

	if (kvm->cfg.nodefaults)
		kvm->cfg.real_cmdline = kvm->cfg.kernel_cmdline;
	else
		kvm_run_set_real_cmdline(kvm);

	if (kvm->cfg.kernel_filename) {
		printf("  # %s run -k %s -m %Lu -c %d --name %s\n", KVM_BINARY_NAME,
		       kvm->cfg.kernel_filename,
		       (unsigned long long)kvm->cfg.ram_size >> MB_SHIFT,
		       kvm->cfg.nrcpus, kvm->cfg.guest_name);
	} else if (kvm->cfg.firmware_filename) {
		printf("  # %s run --firmware %s -m %Lu -c %d --name %s\n", KVM_BINARY_NAME,
		       kvm->cfg.firmware_filename,
		       (unsigned long long)kvm->cfg.ram_size >> MB_SHIFT,
		       kvm->cfg.nrcpus, kvm->cfg.guest_name);
	}

	if (init_list__init(kvm) < 0)
		die ("Initialisation failed");

	return kvm;
}

static int kvm_cmd_run_work(struct kvm *kvm)
{
	int i;

	for (i = 0; i < kvm->nrcpus; i++) {
		if (pthread_create(&kvm->cpus[i]->thread, NULL, kvm_cpu_thread, kvm->cpus[i]) != 0)
			die("unable to create KVM VCPU thread");
	}

	/* Only VCPU #0 is going to exit by itself when shutting down */
	if (pthread_join(kvm->cpus[0]->thread, NULL) != 0)
		die("unable to join with vcpu 0");

	return kvm_cpu__exit(kvm);
}

static void kvm_cmd_run_exit(struct kvm *kvm, int guest_ret)
{
	compat__print_all_messages();

	init_list__exit(kvm);

	if (guest_ret == 0)
		printf("\n  # KVM session ended normally.\n");
}

int kvm_cmd_run(int argc, const char **argv, const char *prefix)
{
	int ret = -EFAULT;
	struct kvm *kvm;

	kvm = kvm_cmd_run_init(argc, argv);
	if (IS_ERR(kvm))
		return PTR_ERR(kvm);

	ret = kvm_cmd_run_work(kvm);
	kvm_cmd_run_exit(kvm, ret);

	return ret;
}
