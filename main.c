#include "kvm/kvm.h"

#include "kvm/early_printk.h"
#include "kvm/util.h"

#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void usage(char *argv[])
{
	fprintf(stderr, "  usage: %s [--single-step] [--params=<kernel-params>] [--kernel=]<kernel-image>\n",
		argv[0]);
	exit(1);
}

static struct kvm *kvm;

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
	const char *kernel_cmdline = NULL;
	bool single_step = false;
	int i;

	signal(SIGQUIT, handle_sigquit);

	for (i = 1; i < argc; i++) {
		if (option_matches(argv[i], "--kernel=")) {
			kernel_filename = &argv[i][9];
			continue;
		} else if (option_matches(argv[i], "--params=")) {
			kernel_cmdline = &argv[i][9];
			continue;
		} else if (option_matches(argv[i], "--single-step")) {
			single_step = true;
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

	kvm = kvm__init();

	kvm__setup_cpuid(kvm);

	strcpy(real_cmdline, "notsc nolapic nosmp noacpi nopci pci=off earlyprintk=serial,keep ");
	if (kernel_cmdline) {
		strlcat(real_cmdline, kernel_cmdline, sizeof(real_cmdline));
		real_cmdline[sizeof(real_cmdline)-1] = '\0';
	}

	if (!kvm__load_kernel(kvm, kernel_filename, real_cmdline))
		die("unable to load kernel %s", kernel_filename);

	kvm__reset_vcpu(kvm);

	if (single_step)
		kvm__enable_singlestep(kvm);

	early_printk__init();

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
				goto exit_kvm;
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
				goto exit_kvm;
			break;

		}
		default:
			goto exit_kvm;
		}
	}

exit_kvm:

	fprintf(stderr, "KVM exit reason: %" PRIu32 " (\"%s\")\n",
		kvm->kvm_run->exit_reason, kvm_exit_reasons[kvm->kvm_run->exit_reason]);
	if (kvm->kvm_run->exit_reason == KVM_EXIT_UNKNOWN)
		fprintf(stderr, "KVM exit code: 0x%016llx\n",
			kvm->kvm_run->hw.hardware_exit_reason);

	kvm__show_registers(kvm);
	kvm__show_code(kvm);
	kvm__show_page_tables(kvm);
out:
	kvm__delete(kvm);

	return 0;
}
