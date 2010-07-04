#include "kvm/kvm.h"

#include "kvm/cpufeature.h"
#include "kvm/util.h"

#include <sys/ioctl.h>
#include <stdlib.h>
#include <assert.h>

struct cpuid_regs {
	uint32_t	eax;
	uint32_t	ebx;
	uint32_t	ecx;
	uint32_t	edx;
};

static inline void host_cpuid(struct cpuid_regs *regs)
{
	asm volatile("cpuid"
		: "=a" (regs->eax),
		  "=b" (regs->ebx),
		  "=c" (regs->ecx),
		  "=d" (regs->edx)
		: "0" (regs->eax), "2" (regs->ecx));
}

static struct kvm_cpuid2 *kvm_cpuid__new(unsigned long nent)
{
	struct kvm_cpuid2 *self;

	self = calloc(1, sizeof(*self) + (sizeof(struct kvm_cpuid_entry2) * nent));
	if (!self)
		die("out of memory");

	return self;
}

static void kvm_cpuid__delete(struct kvm_cpuid2 *self)
{
	free(self);
}

#define	MAX_KVM_CPUID_ENTRIES		100

enum {
	CPUID_GET_VENDOR_ID		= 0x00,
	CPUID_GET_HIGHEST_EXT_FUNCTION	= 0x80000000,
};

static uint32_t cpuid_highest_ext_func(void)
{
	struct cpuid_regs regs;

	regs	= (struct cpuid_regs) {
		.eax		= CPUID_GET_HIGHEST_EXT_FUNCTION,
	};
	host_cpuid(&regs);

	return regs.eax;
}

static uint32_t cpuid_highest_func(void)
{
	struct cpuid_regs regs;

	regs	= (struct cpuid_regs) {
		.eax		= CPUID_GET_VENDOR_ID,
	};
	host_cpuid(&regs);

	return regs.eax;
}

void kvm__setup_cpuid(struct kvm *self)
{
	struct kvm_cpuid2 *kvm_cpuid;
	uint32_t highest_ext;
	uint32_t function;
	uint32_t highest;
	uint32_t ndx = 0;

	kvm_cpuid	= kvm_cpuid__new(MAX_KVM_CPUID_ENTRIES);
	highest		= cpuid_highest_func();

	for (function = 0; function <= highest; function++) {
		/*
		 * NOTE NOTE NOTE! Functions 0x0b and 0x0d seem to need special
		 * treatment as per qemu sources but we treat them as regular
		 * CPUID functions here because they are fairly exotic and the
		 * Linux kernel is not interested in them during boot up.
		 */
		switch (function) {
		case 0x00: {	/* Vendor-ID and Largest Standard Function */
			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= 0,
				.index		= 0,
				.flags		= 0,
				.eax		= 4,
				.ebx		= CPUID_VENDOR_INTEL_1,
				.ecx		= CPUID_VENDOR_INTEL_3,
				.edx		= CPUID_VENDOR_INTEL_2,
			};
			break;
		}
		case 0x01: {	/* Feature Information */
			struct cpuid_regs regs;

			regs	= (struct cpuid_regs) {
				.eax		= function,
			};
			host_cpuid(&regs);

			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 0,
				.flags		= 0,
				.eax		= regs.eax,
				.ebx		= regs.ebx,
				.ecx		=
					cpu_feature_disable(regs.ecx, KVM__X86_FEATURE_XSAVE) &
					cpu_feature_disable(regs.ecx, KVM__X86_FEATURE_VMX),
				.edx		= regs.edx,
			};
			break;
		}
		case 0x02: {	/* Processor configuration descriptor */
			struct cpuid_regs regs;
			uint32_t times;

			regs	= (struct cpuid_regs) {
				.eax		= function,
			};
			host_cpuid(&regs);

			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 0,
				.flags		= KVM_CPUID_FLAG_STATEFUL_FUNC | KVM_CPUID_FLAG_STATE_READ_NEXT,
				.eax		= regs.eax,
				.ebx		= regs.ebx,
				.ecx		= regs.ecx,
				.edx		= regs.edx,
			};

			times	= regs.eax & 0xff;	/* AL */

			while (times-- > 0) {
				regs	= (struct cpuid_regs) {
					.eax		= function,
				};
				host_cpuid(&regs);

				kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
					.function	= function,
					.index		= 0,
					.flags		= KVM_CPUID_FLAG_STATEFUL_FUNC,
					.eax		= regs.eax,
					.ebx		= regs.ebx,
					.ecx		= regs.ecx,
					.edx		= regs.edx,
				};
			}
			break;
		}
		case 0x04: { /* Deterministic Cache Parameters */
			uint32_t eax;
			/*
			 * eax for n cores
			 *     eax = (n - 1) << 26
			 * eax for k threads
			 *     eax = (k - 1) << 14
			 * they could be OR'ified
			 */
			eax = 0;

			/* L1 dcache info */
			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 0,
				.flags		= KVM_CPUID_FLAG_SIGNIFCANT_INDEX | KVM_CPUID_FLAG_STATE_READ_NEXT,
				.eax		= eax | 0x0000121,
				.ebx		= 0x1c0003f,
				.ecx		= 0x000003f,
				.edx		= 0x0000001,
			};

			/* L1 icache info */
			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 1,
				.flags		= KVM_CPUID_FLAG_SIGNIFCANT_INDEX | KVM_CPUID_FLAG_STATE_READ_NEXT,
				.eax		= eax | 0x0000122,
				.ebx		= 0x1c0003f,
				.ecx		= 0x000003f,
				.edx		= 0x0000001,
			};

			/* L2 cache info */
			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 2,
				.flags		= KVM_CPUID_FLAG_SIGNIFCANT_INDEX | KVM_CPUID_FLAG_STATE_READ_NEXT,
				.eax		= eax | 0x0000143,
				.ebx		= 0x3c0003f,
				.ecx		= 0x0000fff,
				.edx		= 0x0000001,
			};

			/* End of list */
			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 3,
				.flags		= 0,
				.eax		= 0,
				.ebx		= 0,
				.ecx		= 0,
				.edx		= 0,
			};
			break;
		}
		default: {
			struct cpuid_regs regs;

			regs	= (struct cpuid_regs) {
				.eax		= function,
			};
			host_cpuid(&regs);

			kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
				.function	= function,
				.index		= 0,
				.flags		= 0,
				.eax		= regs.eax,
				.ebx		= regs.ebx,
				.ecx		= regs.ecx,
				.edx		= regs.edx,
			};
			break;
		}};
	}

	highest_ext	= cpuid_highest_ext_func();

	for (function = CPUID_GET_HIGHEST_EXT_FUNCTION; function <= highest_ext; function++) {
		struct cpuid_regs regs;

		regs	= (struct cpuid_regs) {
			.eax		= function,
		};
		host_cpuid(&regs);

		kvm_cpuid->entries[ndx++]	= (struct kvm_cpuid_entry2) {
			.function	= function,
			.index		= 0,
			.flags		= 0,
			.eax		= regs.eax,
			.ebx		= regs.ebx,
			.ecx		= regs.ecx,
			.edx		= regs.edx,
		};
	}

	assert(ndx < MAX_KVM_CPUID_ENTRIES);

	kvm_cpuid->nent		= ndx;

	if (ioctl(self->vcpu_fd, KVM_SET_CPUID2, kvm_cpuid) < 0)
		die_perror("KVM_SET_CPUID2 failed");

	kvm_cpuid__delete(kvm_cpuid);
}
