#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <termios.h>

/* user defined header files */
#include <linux/types.h>
#include <kvm/kvm.h>
#include <kvm/kvm-cpu.h>
#include <kvm/8250-serial.h>
#include <kvm/virtio-blk.h>
#include <kvm/virtio-console.h>
#include <kvm/disk-image.h>
#include <kvm/util.h>
#include <kvm/pci.h>
#include <kvm/term.h>

/* header files for gitish interface  */
#include <kvm/kvm-run.h>
#include <kvm/parse-options.h>

#define DEFAULT_KVM_DEV		"/dev/kvm"

#define MB_SHIFT		(20)
#define MIN_RAM_SIZE_MB		(64ULL)
#define MIN_RAM_SIZE_BYTE	(MIN_RAM_SIZE_MB << MB_SHIFT)

#define KVM_NR_CPUS		(255)

static struct kvm *kvm;
static struct kvm_cpu *kvm_cpus[KVM_NR_CPUS];
static __thread struct kvm_cpu *current_kvm_cpu;

static void handle_sigint(int sig)
{
	exit(1);
}

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
static const char *kvm_dev;
static bool single_step;
static bool virtio_console;
extern bool ioport_debug;
extern int  active_console;

static int nrcpus = 1;

static const char * const run_usage[] = {
	"kvm run [<options>] <kernel image>",
	NULL
};

static const struct option options[] = {
	OPT_U64('m', "mem", &ram_size, "Virtual machine memory size in MiB."),
	OPT_STRING('p', "params", &kernel_cmdline, "params",
			"Kernel command line arguments"),
	OPT_STRING('r', "initrd", &initrd_filename, "initrd",
			"Initial RAM disk image"),
	OPT_STRING('k', "kernel", &kernel_filename, "kernel",
			"Kernel to boot in virtual machine"),
	OPT_STRING('i', "image", &image_filename, "image", "Disk image"),
	OPT_STRING('d', "kvm-dev", &kvm_dev, "kvm-dev", "KVM device file"),
	OPT_BOOLEAN('s', "single-step", &single_step,
			"Enable single stepping"),
	OPT_BOOLEAN('g', "ioport-debug", &ioport_debug,
			"Enable ioport debugging"),
	OPT_BOOLEAN('c', "enable-virtio-console", &virtio_console,
			"Enable the virtual IO console"),
	OPT_INTEGER('\0', "cpus", &nrcpus, "Number of CPUs"),
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

int kvm_cmd_run(int argc, const char **argv, const char *prefix)
{
	static char real_cmdline[2048];
	int exit_code = 0;
	int i;

	signal(SIGALRM, handle_sigalrm);
	signal(SIGQUIT, handle_sigquit);
	signal(SIGINT, handle_sigint);

	if (!argv || !*argv) {
		/* no argument specified */
		usage_with_options(run_usage, options);
		return EINVAL;
	}

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

	if (virtio_console == true)
		active_console  = CONSOLE_VIRTIO;

	term_init();

	kvm = kvm__init(kvm_dev, ram_size);

	if (image_filename) {
		kvm->disk_image	= disk_image__open(image_filename);
		if (!kvm->disk_image)
			die("unable to load disk image %s", image_filename);
	}

	strcpy(real_cmdline, "notsc nolapic noacpi pci=conf1 console=ttyS0 ");
	if (!kernel_cmdline || !strstr(kernel_cmdline, "root=")) {
		strlcat(real_cmdline, "root=/dev/vda rw ",
				sizeof(real_cmdline));
	}

	if (kernel_cmdline) {
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));
		real_cmdline[sizeof(real_cmdline)-1] = '\0';
	}

	if (!kvm__load_kernel(kvm, kernel_filename, initrd_filename,
				real_cmdline))
		die("unable to load kernel %s", kernel_filename);

	kvm__setup_bios(kvm);

	serial8250__init(kvm);

	pci__init();

	virtio_blk__init(kvm);

	virtio_console__init(kvm);

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
