#ifndef KVM__KVM_H
#define KVM__KVM_H

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

	struct kvm_regs		regs;
};

struct kvm *kvm__init(void);
void kvm__enable_singlestep(struct kvm *self);
uint32_t kvm__load_kernel(struct kvm *kvm, const char *kernel_filename);
void kvm__reset_vcpu(struct kvm *self, uint64_t rip);
void kvm__run(struct kvm *self);
void kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count);

/*
 * Debugging
 */
void kvm__show_code(struct kvm *self);
void kvm__show_registers(struct kvm *self);

extern const char *kvm_exit_reasons[];

#endif /* KVM__KVM_H */
