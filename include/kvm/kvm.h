#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/interrupt.h"

#include <linux/kvm.h>	/* for struct kvm_regs */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	int			vcpu_fd;	/* For VCPU ioctls() */
	timer_t			timerid;	/* Posix timer for interrupts */
	struct kvm_run		*kvm_run;

	struct disk_image	*disk_image;
	uint64_t		ram_size;
	void			*ram_start;

	bool			nmi_disabled;

	uint16_t		boot_selector;
	uint16_t		boot_ip;
	uint16_t		boot_sp;

	struct kvm_regs		regs;
	struct kvm_sregs	sregs;
	struct kvm_fpu		fpu;
	struct kvm_msrs		*msrs;	/* dynamically allocated */

	struct interrupt_table	interrupt_table;
};

struct kvm *kvm__init(const char *kvm_dev, unsigned long ram_size);
void kvm__delete(struct kvm *self);
void kvm__setup_cpuid(struct kvm *self);
void kvm__enable_singlestep(struct kvm *self);
bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
			const char *initrd_filename, const char *kernel_cmdline);
void kvm__reset_vcpu(struct kvm *self);
void kvm__setup_bios(struct kvm *self);
void kvm__start_timer(struct kvm *self);
void kvm__run(struct kvm *self);
void kvm__irq_line(struct kvm *self, int irq, int level);
bool kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count);
bool kvm__emulate_mmio(struct kvm *self, uint64_t phys_addr, uint8_t *data, uint32_t len, uint8_t is_write);

/*
 * Debugging
 */
void kvm__show_code(struct kvm *self);
void kvm__show_registers(struct kvm *self);
void kvm__show_page_tables(struct kvm *self);
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
