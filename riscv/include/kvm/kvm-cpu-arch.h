#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

#include <linux/kvm.h>
#include <pthread.h>
#include <stdbool.h>

#include "kvm/kvm.h"

static inline __u64 __kvm_reg_id(__u64 type, __u64 idx, __u64  size)
{
	return KVM_REG_RISCV | type | idx | size;
}

#if __riscv_xlen == 64
#define KVM_REG_SIZE_ULONG	KVM_REG_SIZE_U64
#else
#define KVM_REG_SIZE_ULONG	KVM_REG_SIZE_U32
#endif

#define RISCV_CONFIG_REG(name)	__kvm_reg_id(KVM_REG_RISCV_CONFIG, \
					     KVM_REG_RISCV_CONFIG_REG(name), \
					     KVM_REG_SIZE_ULONG)

#define RISCV_ISA_EXT_REG(id)	__kvm_reg_id(KVM_REG_RISCV_ISA_EXT, \
					     id, KVM_REG_SIZE_ULONG)

#define RISCV_CORE_REG(name)	__kvm_reg_id(KVM_REG_RISCV_CORE, \
					     KVM_REG_RISCV_CORE_REG(name), \
					     KVM_REG_SIZE_ULONG)

#define RISCV_CSR_REG(name)	__kvm_reg_id(KVM_REG_RISCV_CSR, \
					     KVM_REG_RISCV_CSR_REG(name), \
					     KVM_REG_SIZE_ULONG)

#define RISCV_TIMER_REG(name)	__kvm_reg_id(KVM_REG_RISCV_TIMER, \
					     KVM_REG_RISCV_TIMER_REG(name), \
					     KVM_REG_SIZE_U64)

struct kvm_cpu {
	pthread_t	thread;

	unsigned long   cpu_id;

	unsigned long	riscv_xlen;
	unsigned long	riscv_isa;
	unsigned long	riscv_timebase;

	struct kvm	*kvm;
	int		vcpu_fd;
	struct kvm_run	*kvm_run;
	struct kvm_cpu_task	*task;

	u8		is_running;
	u8		paused;
	u8		needs_nmi;

	struct kvm_coalesced_mmio_ring	*ring;
};

static inline bool kvm_cpu__emulate_io(struct kvm_cpu *vcpu, u16 port,
				       void *data, int direction,
				       int size, u32 count)
{
	return false;
}

static inline bool kvm_cpu__emulate_mmio(struct kvm_cpu *vcpu, u64 phys_addr,
					 u8 *data, u32 len, u8 is_write)
{
	if (riscv_addr_in_ioport_region(phys_addr)) {
		int direction = is_write ? KVM_EXIT_IO_OUT : KVM_EXIT_IO_IN;
		u16 port = (phys_addr - KVM_IOPORT_AREA) & USHRT_MAX;

		return kvm__emulate_io(vcpu, port, data, direction, len, 1);
	}

	return kvm__emulate_mmio(vcpu, phys_addr, data, len, is_write);
}

#endif /* KVM__KVM_CPU_ARCH_H */
