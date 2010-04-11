#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/interrupt.h"

#include <linux/kvm.h>	/* for struct kvm_regs */

#include <stdbool.h>
#include <stdint.h>

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	int			vcpu_fd;	/* For VCPU ioctls() */
	struct kvm_run		*kvm_run;

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

struct kvm *kvm__init(void);
void kvm__delete(struct kvm *self);
void kvm__setup_cpuid(struct kvm *self);
void kvm__enable_singlestep(struct kvm *self);
bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename, const char *kernel_cmdline);
void kvm__reset_vcpu(struct kvm *self);
void kvm__run(struct kvm *self);
bool kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count);

/*
 * Debugging
 */
void kvm__show_code(struct kvm *self);
void kvm__show_registers(struct kvm *self);
void kvm__show_page_tables(struct kvm *self);
void kvm__dump_mem(struct kvm *self, unsigned long addr, unsigned long size);

extern const char *kvm_exit_reasons[];

#endif /* KVM__KVM_H */
