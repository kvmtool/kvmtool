#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include "kvm/interrupt.h"
#include "kvm/segment.h"

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>

/*
 * The hole includes VESA framebuffer and PCI memory.
 */
#define KVM_32BIT_MAX_MEM_SIZE  (1ULL << 32)
#define KVM_32BIT_GAP_SIZE	(768 << 20)
#define KVM_32BIT_GAP_START	(KVM_32BIT_MAX_MEM_SIZE - KVM_32BIT_GAP_SIZE)

#define KVM_MMIO_START		KVM_32BIT_GAP_START

/* This is the address that pci_get_io_space_block() starts allocating
 * from.  Note that this is a PCI bus address (though same on x86).
 */
#define KVM_PCI_MMIO_AREA	(KVM_MMIO_START + 0x1000000)

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

	char			*name;

	int			vm_state;
};

static inline void *guest_flat_to_host(struct kvm *kvm, unsigned long offset); /* In kvm.h */

static inline void *guest_real_to_host(struct kvm *kvm, u16 selector, u16 offset)
{
	unsigned long flat = segment_to_flat(selector, offset);

	return guest_flat_to_host(kvm, flat);
}

#endif /* KVM__KVM_ARCH_H */
