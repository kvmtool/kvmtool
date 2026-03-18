#ifndef KVM__GDB_H
#define KVM__GDB_H

#include <stdbool.h>
#include <stddef.h>
#include <linux/types.h>

struct kvm;
struct kvm_cpu;

/* Hardware breakpoint descriptor (shared with arch-specific code) */
struct kvm_gdb_hw_bp {
	u64  addr;
	int  len;	/* 1, 2, 4, or 8 bytes */
	int  type;	/* 0=exec, 1=write, 2=read, 3=access */
	bool active;
};

#if defined(CONFIG_X86) || defined(CONFIG_ARM64)

/*
 * Public GDB stub API
 */

/* Initialize and start the GDB stub (called from late_init) */
int kvm_gdb__init(struct kvm *kvm);

/* Shutdown the GDB stub */
int kvm_gdb__exit(struct kvm *kvm);

/* Called by kvm_cpu__start() when KVM_EXIT_DEBUG occurs */
void kvm_gdb__handle_debug(struct kvm_cpu *vcpu);

/* Returns true when a GDB stub is active on this VM */
bool kvm_gdb__active(struct kvm *kvm);

/*
 * Architecture-specific callbacks (implemented per-arch, e.g. x86/gdb.c)
 */

/* Read all registers into buf, set *size to number of bytes written */
void kvm_gdb__arch_read_registers(struct kvm_cpu *vcpu, u8 *buf,
				   size_t *size);

/* Write all registers from buf (size bytes) */
void kvm_gdb__arch_write_registers(struct kvm_cpu *vcpu, const u8 *buf,
				    size_t size);

/* Read a single register (GDB regno) into buf, set *size */
int kvm_gdb__arch_read_register(struct kvm_cpu *vcpu, int regno,
				 u8 *buf, size_t *size);

/* Write a single register (GDB regno) from buf (size bytes) */
int kvm_gdb__arch_write_register(struct kvm_cpu *vcpu, int regno,
				  const u8 *buf, size_t size);

/* Return current PC of the vCPU */
u64 kvm_gdb__arch_get_pc(struct kvm_cpu *vcpu);

/* Set PC of the vCPU */
void kvm_gdb__arch_set_pc(struct kvm_cpu *vcpu, u64 pc);

/*
 * Enable/disable guest debugging on a vCPU.
 *  single_step: true  -> enable instruction-level single-step
 *  hw_bps:      array of 4 hardware breakpoints (may be NULL)
 */
void kvm_gdb__arch_set_debug(struct kvm_cpu *vcpu, bool single_step,
			      struct kvm_gdb_hw_bp *hw_bps);

/*
 * Prepare guest architectural state before resuming from a GDB stop.
 * from_debug_exit is true when the current stop came from KVM_EXIT_DEBUG.
 */
void kvm_gdb__arch_prepare_resume(struct kvm_cpu *vcpu, bool single_step,
				   bool from_debug_exit);

/*
 * Called when a KVM_EXIT_DEBUG stop is selected for a GDB session.
 * Arch code can restore temporary state applied for stepping.
 */
void kvm_gdb__arch_handle_stop(struct kvm_cpu *vcpu);

/* Return the GDB target XML description string (NULL-terminated) */
const char *kvm_gdb__arch_target_xml(void);

/* Total byte size of the 'g' register packet */
size_t kvm_gdb__arch_reg_pkt_size(void);

/* GDB signal number to report on stop (SIGTRAP=5) */
int kvm_gdb__arch_signal(struct kvm_cpu *vcpu);

/*
 * Returns true if the KVM_EXIT_DEBUG exit was caused by a software
 * breakpoint (INT3 / #BP exception), as opposed to a hardware debug
 * trap (#DB, single-step, hardware breakpoint).
 */
bool kvm_gdb__arch_is_sw_bp_exit(struct kvm_cpu *vcpu);

/*
 * Returns the guest virtual address of the INT3 instruction that triggered
 * the current software-breakpoint exit (i.e. the byte that holds 0xCC).
 * Only meaningful when kvm_gdb__arch_is_sw_bp_exit() returns true.
 */
u64 kvm_gdb__arch_debug_pc(struct kvm_cpu *vcpu);

/*
 * Re-inject the #BP exception back into the guest so that the guest's
 * own INT3 handler (e.g. kernel jump-label patching, int3_selftest) sees
 * it instead of us treating it as a GDB breakpoint.
 * Only meaningful when kvm_gdb__arch_is_sw_bp_exit() returns true.
 */
void kvm_gdb__arch_reinject_sw_bp(struct kvm_cpu *vcpu);

#else

static inline int kvm_gdb__init(struct kvm *kvm)
{
	return 0;
}

static inline int kvm_gdb__exit(struct kvm *kvm)
{
	return 0;
}

static inline void kvm_gdb__handle_debug(struct kvm_cpu *vcpu)
{
}

static inline bool kvm_gdb__active(struct kvm *kvm)
{
	return false;
}

#endif

#endif /* KVM__GDB_H */
