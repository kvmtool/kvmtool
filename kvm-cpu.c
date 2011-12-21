#include "kvm/kvm-cpu.h"

#include "kvm/symbol.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

extern struct kvm_cpu **kvm_cpus;
extern __thread struct kvm_cpu *current_kvm_cpu;

void kvm_cpu__enable_singlestep(struct kvm_cpu *vcpu)
{
	struct kvm_guest_debug debug = {
		.control	= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP,
	};

	if (ioctl(vcpu->vcpu_fd, KVM_SET_GUEST_DEBUG, &debug) < 0)
		pr_warning("KVM_SET_GUEST_DEBUG failed");
}

void kvm_cpu__run(struct kvm_cpu *vcpu)
{
	int err;

	if (!vcpu->is_running)
		return;

	err = ioctl(vcpu->vcpu_fd, KVM_RUN, 0);
	if (err < 0 && (errno != EINTR && errno != EAGAIN))
		die_perror("KVM_RUN failed");
}

static void kvm_cpu_signal_handler(int signum)
{
	if (signum == SIGKVMEXIT) {
		if (current_kvm_cpu && current_kvm_cpu->is_running) {
			current_kvm_cpu->is_running = false;
			kvm__continue();
		}
	} else if (signum == SIGKVMPAUSE) {
		current_kvm_cpu->paused = 1;
	}
}

static void kvm_cpu__handle_coalesced_mmio(struct kvm_cpu *cpu)
{
	if (cpu->ring) {
		while (cpu->ring->first != cpu->ring->last) {
			struct kvm_coalesced_mmio *m;
			m = &cpu->ring->coalesced_mmio[cpu->ring->first];
			kvm_cpu__emulate_mmio(cpu->kvm,
					      m->phys_addr,
					      m->data,
					      m->len,
					      1);
			cpu->ring->first = (cpu->ring->first + 1) % KVM_COALESCED_MMIO_MAX;
		}
	}
}

void kvm_cpu__reboot(void)
{
	int i;

	/* The kvm_cpus array contains a null pointer in the last location */
	for (i = 0; ; i++) {
		if (kvm_cpus[i])
			pthread_kill(kvm_cpus[i]->thread, SIGKVMEXIT);
		else
			break;
	}
}

int kvm_cpu__start(struct kvm_cpu *cpu)
{
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);

	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	signal(SIGKVMEXIT, kvm_cpu_signal_handler);
	signal(SIGKVMPAUSE, kvm_cpu_signal_handler);

	kvm_cpu__reset_vcpu(cpu);

	if (cpu->kvm->single_step)
		kvm_cpu__enable_singlestep(cpu);

	while (cpu->is_running) {
		if (cpu->paused) {
			kvm__notify_paused();
			cpu->paused = 0;
		}

		if (cpu->needs_nmi) {
			kvm_cpu__arch_nmi(cpu);
			cpu->needs_nmi = 0;
		}

		kvm_cpu__run(cpu);

		switch (cpu->kvm_run->exit_reason) {
		case KVM_EXIT_UNKNOWN:
			break;
		case KVM_EXIT_DEBUG:
			kvm_cpu__show_registers(cpu);
			kvm_cpu__show_code(cpu);
			break;
		case KVM_EXIT_IO: {
			bool ret;

			ret = kvm_cpu__emulate_io(cpu->kvm,
						  cpu->kvm_run->io.port,
						  (u8 *)cpu->kvm_run +
						  cpu->kvm_run->io.data_offset,
						  cpu->kvm_run->io.direction,
						  cpu->kvm_run->io.size,
						  cpu->kvm_run->io.count);

			if (!ret)
				goto panic_kvm;
			break;
		}
		case KVM_EXIT_MMIO: {
			bool ret;

			/*
			 * If we had MMIO exit, coalesced ring should be processed
			 * *before* processing the exit itself
			 */
			kvm_cpu__handle_coalesced_mmio(cpu);

			ret = kvm_cpu__emulate_mmio(cpu->kvm,
						    cpu->kvm_run->mmio.phys_addr,
						    cpu->kvm_run->mmio.data,
						    cpu->kvm_run->mmio.len,
						    cpu->kvm_run->mmio.is_write);

			if (!ret)
				goto panic_kvm;
			break;
		}
		case KVM_EXIT_INTR:
			if (cpu->is_running)
				break;
			goto exit_kvm;
		case KVM_EXIT_SHUTDOWN:
			goto exit_kvm;
		default: {
			bool ret;

			ret = kvm_cpu__handle_exit(cpu);
			if (!ret)
				goto panic_kvm;
			break;
		}
		}
		kvm_cpu__handle_coalesced_mmio(cpu);
	}

exit_kvm:
	return 0;

panic_kvm:
	return 1;
}
