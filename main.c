#include "kvm/kvm.h"

#include "kvm/8250-serial.h"
#include "kvm/blk-virtio.h"
#include "kvm/console-virtio.h"
#include "kvm/disk-image.h"
#include "kvm/util.h"
#include "kvm/pci.h"
#include "kvm/term.h"

#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

extern bool ioport_debug;

static void usage(char *argv[])
{
	fprintf(stderr, "  usage: %s "
		"[--single-step] [--ioport-debug] "
		"[--kvm-dev=<device>] [--mem=<size-in-MiB>] [--params=<kernel-params>] "
		"[--initrd=<initrd>] [--kernel=]<kernel-image> [--image=]<disk-image>\n",
		argv[0]);
	exit(1);
}

static struct kvm *kvm;

static void handle_sigint(int sig)
{
	exit(1);
}

static void handle_sigquit(int sig)
{
	kvm__show_registers(kvm);
	kvm__show_code(kvm);
	kvm__show_page_tables(kvm);
	kvm__delete(kvm);

	exit(1);
}

static char real_cmdline[2048];

static bool option_matches(char *arg, const char *option)
{
	return !strncmp(arg, option, strlen(option));
}

int main(int argc, char *argv[])
{
	const char *kernel_filename = NULL;
	const char *initrd_filename = NULL;
	const char *image_filename = NULL;
	const char *kernel_cmdline = NULL;
	const char *kvm_dev = "/dev/kvm";
	unsigned long ram_size = 64UL << 20;
	bool single_step = false;
	int i;

	signal(SIGQUIT, handle_sigquit);
	signal(SIGINT, handle_sigint);

	for (i = 1; i < argc; i++) {
		if (option_matches(argv[i], "--kernel=")) {
			kernel_filename	= &argv[i][9];
			continue;
		} else if (option_matches(argv[i], "--image=")) {
			image_filename	= &argv[i][8];
			continue;
		} else if (option_matches(argv[i], "--initrd=")) {
			initrd_filename	= &argv[i][9];
			continue;
		} else if (option_matches(argv[i], "--params=")) {
			kernel_cmdline	= &argv[i][9];
			continue;
		} else if (option_matches(argv[i], "--kvm-dev=")) {
			kvm_dev		= &argv[i][10];
			continue;
		} else if (option_matches(argv[i], "--single-step")) {
			single_step	= true;
			continue;
		} else if (option_matches(argv[i], "--mem=")) {
			unsigned long val = atol(&argv[i][6]) << 20;
			if (val < ram_size)
				die("Not enough memory specified: %sMB (min %luMB)",
					argv[i], ram_size >> 20);
			ram_size = val;
			continue;
		} else if (option_matches(argv[i], "--ioport-debug")) {
			ioport_debug	= true;
			continue;
		} else {
			/* any unspecified arg is kernel image */
			if (argv[i][0] != '-')
				kernel_filename = argv[i];
			else
				warning("Unknown option: %s", argv[i]);
		}
	}

	/* at least we should have kernel image passed */
	if (!kernel_filename)
		usage(argv);

	term_init();

	kvm = kvm__init(kvm_dev, ram_size);

	if (image_filename) {
		kvm->disk_image	= disk_image__open(image_filename);
		if (!kvm->disk_image)
			die("unable to load disk image %s", image_filename);
	}

	kvm__setup_cpuid(kvm);

	strcpy(real_cmdline, "notsc nolapic noacpi pci=conf1 console=ttyS0 ");
	if (!kernel_cmdline || !strstr(kernel_cmdline, "root="))
		strlcat(real_cmdline, "root=/dev/vda rw ", sizeof(real_cmdline));

	if (kernel_cmdline) {
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));
		real_cmdline[sizeof(real_cmdline)-1] = '\0';
	}

	if (!kvm__load_kernel(kvm, kernel_filename, initrd_filename, real_cmdline))
		die("unable to load kernel %s", kernel_filename);

	kvm__reset_vcpu(kvm);

	kvm__setup_bios(kvm);

	if (single_step)
		kvm__enable_singlestep(kvm);

	serial8250__init(kvm);

	pci__init();

	blk_virtio__init(kvm);

	virtio_console__init(kvm);

	kvm__start_timer(kvm);

	for (;;) {
		kvm__run(kvm);

		switch (kvm->kvm_run->exit_reason) {
		case KVM_EXIT_DEBUG:
			kvm__show_registers(kvm);
			kvm__show_code(kvm);
			break;
		case KVM_EXIT_IO: {
			bool ret;

			ret = kvm__emulate_io(kvm,
					kvm->kvm_run->io.port,
					(uint8_t *)kvm->kvm_run + kvm->kvm_run->io.data_offset,
					kvm->kvm_run->io.direction,
					kvm->kvm_run->io.size,
					kvm->kvm_run->io.count);

			if (!ret)
				goto panic_kvm;
			break;
		}
		case KVM_EXIT_MMIO: {
			bool ret;

			ret = kvm__emulate_mmio(kvm,
					kvm->kvm_run->mmio.phys_addr,
					kvm->kvm_run->mmio.data,
					kvm->kvm_run->mmio.len,
					kvm->kvm_run->mmio.is_write);

			if (!ret)
				goto panic_kvm;
			break;
		}
		case KVM_EXIT_INTR: {
			serial8250__inject_interrupt(kvm);
			virtio_console__inject_interrupt(kvm);
			break;
		}
		case KVM_EXIT_SHUTDOWN:
			goto exit_kvm;
		default:
			goto panic_kvm;
		}
	}
exit_kvm:
	disk_image__close(kvm->disk_image);
	kvm__delete(kvm);

	return 0;

panic_kvm:
	fprintf(stderr, "KVM exit reason: %" PRIu32 " (\"%s\")\n",
		kvm->kvm_run->exit_reason, kvm_exit_reasons[kvm->kvm_run->exit_reason]);
	if (kvm->kvm_run->exit_reason == KVM_EXIT_UNKNOWN)
		fprintf(stderr, "KVM exit code: 0x%" PRIu64 "\n",
			kvm->kvm_run->hw.hardware_exit_reason);
	disk_image__close(kvm->disk_image);
	kvm__show_registers(kvm);
	kvm__show_code(kvm);
	kvm__show_page_tables(kvm);
	kvm__delete(kvm);

	return 1;
}
