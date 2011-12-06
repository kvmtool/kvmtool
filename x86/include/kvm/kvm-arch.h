#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include "kvm/interrupt.h"
#include "kvm/segment.h"

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>

#define KVM_NR_CPUS		(255)

/*
 * The hole includes VESA framebuffer and PCI memory.
 */
#define KVM_32BIT_GAP_SIZE	(768 << 20)
#define KVM_32BIT_GAP_START	((1ULL << 32) - KVM_32BIT_GAP_SIZE)

#define KVM_MMIO_START		KVM_32BIT_GAP_START

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	timer_t			timerid;	/* Posix timer for interrupts */

	int			nrcpus;		/* Number of cpus to run */

	u32			mem_slots;	/* for KVM_SET_USER_MEMORY_REGION */

	u64			ram_size;
	void			*ram_start;

	bool			nmi_disabled;

	bool			single_step;

	u16			boot_selector;
	u16			boot_ip;
	u16			boot_sp;

	struct interrupt_table	interrupt_table;

	const char		*vmlinux;
	struct disk_image       **disks;
	int                     nr_disks;

	const char		*name;
};

static inline void *guest_flat_to_host(struct kvm *kvm, unsigned long offset); /* In kvm.h */

static inline void *guest_real_to_host(struct kvm *kvm, u16 selector, u16 offset)
{
	unsigned long flat = segment_to_flat(selector, offset);

	return guest_flat_to_host(kvm, flat);
}

#endif /* KVM__KVM_ARCH_H */
