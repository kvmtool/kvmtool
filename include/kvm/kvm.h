#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/interrupt.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define KVM_NR_CPUS		(255)

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	timer_t			timerid;	/* Posix timer for interrupts */

	int			nrcpus;		/* Number of cpus to run */

	uint64_t		ram_size;
	void			*ram_start;

	bool			nmi_disabled;

	uint16_t		boot_selector;
	uint16_t		boot_ip;
	uint16_t		boot_sp;

	struct interrupt_table	interrupt_table;
};

struct kvm *kvm__init(const char *kvm_dev, unsigned long ram_size);
void kvm__init_ram(struct kvm *self);
void kvm__delete(struct kvm *self);
bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
			const char *initrd_filename, const char *kernel_cmdline);
void kvm__setup_bios(struct kvm *self);
void kvm__start_timer(struct kvm *self);
void kvm__stop_timer(struct kvm *self);
void kvm__irq_line(struct kvm *self, int irq, int level);
bool kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count);
bool kvm__emulate_mmio(struct kvm *self, uint64_t phys_addr, uint8_t *data, uint32_t len, uint8_t is_write);

/*
 * Debugging
 */
void kvm__dump_mem(struct kvm *self, unsigned long addr, unsigned long size);

extern const char *kvm_exit_reasons[];

static inline bool host_ptr_in_ram(struct kvm *self, void *p)
{
	return self->ram_start <= p && p < (self->ram_start + self->ram_size);
}

static inline uint32_t segment_to_flat(uint16_t selector, uint16_t offset)
{
	return ((uint32_t)selector << 4) + (uint32_t) offset;
}

static inline void *guest_flat_to_host(struct kvm *self, unsigned long offset)
{
	return self->ram_start + offset;
}

static inline void *guest_real_to_host(struct kvm *self, uint16_t selector, uint16_t offset)
{
	unsigned long flat = segment_to_flat(selector, offset);

	return guest_flat_to_host(self, flat);
}

#endif /* KVM__KVM_H */
