#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/interrupt.h"

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>
#include <signal.h>

#define KVM_NR_CPUS		(255)

/*
 * The hole includes VESA framebuffer and PCI memory.
 */
#define KVM_32BIT_GAP_SIZE	(768 << 20)
#define KVM_32BIT_GAP_START	((1ULL << 32) - KVM_32BIT_GAP_SIZE)

#define SIGKVMEXIT		(SIGRTMIN + 0)
#define SIGKVMPAUSE		(SIGRTMIN + 1)
#define SIGKVMADDMEM		(SIGRTMIN + 2)
#define SIGKVMDELMEM		(SIGRTMIN + 3)
#define SIGKVMSTOP		(SIGRTMIN + 4)
#define SIGKVMRESUME		(SIGRTMIN + 5)
#define SIGKVMMEMSTAT		(SIGRTMIN + 6)

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

struct kvm *kvm__init(const char *kvm_dev, u64 ram_size, const char *name);
int kvm__recommended_cpus(struct kvm *kvm);
int kvm__max_cpus(struct kvm *kvm);
void kvm__init_ram(struct kvm *kvm);
void kvm__delete(struct kvm *kvm);
bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
			const char *initrd_filename, const char *kernel_cmdline, u16 vidmode);
void kvm__setup_bios(struct kvm *kvm);
void kvm__start_timer(struct kvm *kvm);
void kvm__stop_timer(struct kvm *kvm);
void kvm__irq_line(struct kvm *kvm, int irq, int level);
void kvm__irq_trigger(struct kvm *kvm, int irq);
bool kvm__emulate_io(struct kvm *kvm, u16 port, void *data, int direction, int size, u32 count);
bool kvm__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write);
void kvm__register_mem(struct kvm *kvm, u64 guest_phys, u64 size, void *userspace_addr);
bool kvm__register_mmio(struct kvm *kvm, u64 phys_addr, u64 phys_addr_len, void (*kvm_mmio_callback_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr), void *ptr);
bool kvm__deregister_mmio(struct kvm *kvm, u64 phys_addr);
void kvm__pause(void);
void kvm__continue(void);
void kvm__notify_paused(void);
int kvm__get_pid_by_instance(const char *name);
int kvm__enumerate_instances(int (*callback)(const char *name, int pid));
void kvm__remove_pidfile(const char *name);

/*
 * Debugging
 */
void kvm__dump_mem(struct kvm *kvm, unsigned long addr, unsigned long size);

extern const char *kvm_exit_reasons[];

static inline bool host_ptr_in_ram(struct kvm *kvm, void *p)
{
	return kvm->ram_start <= p && p < (kvm->ram_start + kvm->ram_size);
}

static inline u32 segment_to_flat(u16 selector, u16 offset)
{
	return ((u32)selector << 4) + (u32) offset;
}

static inline void *guest_flat_to_host(struct kvm *kvm, unsigned long offset)
{
	return kvm->ram_start + offset;
}

static inline void *guest_real_to_host(struct kvm *kvm, u16 selector, u16 offset)
{
	unsigned long flat = segment_to_flat(selector, offset);

	return guest_flat_to_host(kvm, flat);
}

#endif /* KVM__KVM_H */
