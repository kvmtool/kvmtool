#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/interrupt.h"

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>

#define KVM_NR_CPUS		(255)

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	timer_t			timerid;	/* Posix timer for interrupts */

	int			nrcpus;		/* Number of cpus to run */

	u64			ram_size;
	void			*ram_start;

	bool			nmi_disabled;

	u16			boot_selector;
	u16			boot_ip;
	u16			boot_sp;

	struct interrupt_table	interrupt_table;
};

struct kvm *kvm__init(const char *kvm_dev, unsigned long ram_size);
int kvm__max_cpus(struct kvm *self);
void kvm__init_ram(struct kvm *self);
void kvm__delete(struct kvm *self);
bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
			const char *initrd_filename, const char *kernel_cmdline);
void kvm__setup_bios(struct kvm *self);
void kvm__start_timer(struct kvm *self);
void kvm__stop_timer(struct kvm *self);
void kvm__irq_line(struct kvm *self, int irq, int level);
bool kvm__emulate_io(struct kvm *self, u16 port, void *data, int direction, int size, u32 count);
bool kvm__emulate_mmio(struct kvm *self, u64 phys_addr, u8 *data, u32 len, u8 is_write);

/*
 * Debugging
 */
void kvm__dump_mem(struct kvm *self, unsigned long addr, unsigned long size);

extern const char *kvm_exit_reasons[];

static inline bool host_ptr_in_ram(struct kvm *self, void *p)
{
	return self->ram_start <= p && p < (self->ram_start + self->ram_size);
}

static inline u32 segment_to_flat(u16 selector, u16 offset)
{
	return ((u32)selector << 4) + (u32) offset;
}

static inline void *guest_flat_to_host(struct kvm *self, unsigned long offset)
{
	return self->ram_start + offset;
}

static inline void *guest_real_to_host(struct kvm *self, u16 selector, u16 offset)
{
	unsigned long flat = segment_to_flat(selector, offset);

	return guest_flat_to_host(self, flat);
}

#endif /* KVM__KVM_H */
