/*
 * SPAPR definitions and declarations
 *
 * Borrowed heavily from QEMU's spapr.h,
 * Copyright (c) 2010 David Gibson, IBM Corporation.
 *
 * Modifications by Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#if !defined(__HW_SPAPR_H__)
#define __HW_SPAPR_H__

#include <inttypes.h>

/* We need some of the H_ hcall defs, but they're __KERNEL__ only. */
#define __KERNEL__
#include <asm/hvcall.h>
#undef __KERNEL__

#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"

typedef unsigned long target_ulong;
typedef uintptr_t target_phys_addr_t;

/*
 * The hcalls above are standardized in PAPR and implemented by pHyp
 * as well.
 *
 * We also need some hcalls which are specific to qemu / KVM-on-POWER.
 * So far we just need one for H_RTAS, but in future we'll need more
 * for extensions like virtio.  We put those into the 0xf000-0xfffc
 * range which is reserved by PAPR for "platform-specific" hcalls.
 */
#define KVMPPC_HCALL_BASE       0xf000
#define KVMPPC_H_RTAS           (KVMPPC_HCALL_BASE + 0x0)
#define KVMPPC_HCALL_MAX        KVMPPC_H_RTAS

#define DEBUG_SPAPR_HCALLS

#ifdef DEBUG_SPAPR_HCALLS
#define hcall_dprintf(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define hcall_dprintf(fmt, ...) \
    do { } while (0)
#endif

typedef target_ulong (*spapr_hcall_fn)(struct kvm_cpu *vcpu,
				       target_ulong opcode,
                                       target_ulong *args);

void hypercall_init(void);
void register_core_rtas(void);

void spapr_register_hypercall(target_ulong opcode, spapr_hcall_fn fn);
target_ulong spapr_hypercall(struct kvm_cpu *vcpu, target_ulong opcode,
                             target_ulong *args);

int spapr_rtas_fdt_setup(struct kvm *kvm, void *fdt);

static inline uint32_t rtas_ld(struct kvm *kvm, target_ulong phys, int n)
{
	return *((uint32_t *)guest_flat_to_host(kvm, phys + 4*n));
}

static inline void rtas_st(struct kvm *kvm, target_ulong phys, int n, uint32_t val)
{
	*((uint32_t *)guest_flat_to_host(kvm, phys + 4*n)) = val;
}

typedef void (*spapr_rtas_fn)(struct kvm_cpu *vcpu, uint32_t token,
                              uint32_t nargs, target_ulong args,
                              uint32_t nret, target_ulong rets);
void spapr_rtas_register(const char *name, spapr_rtas_fn fn);
target_ulong spapr_rtas_call(struct kvm_cpu *vcpu,
                             uint32_t token, uint32_t nargs, target_ulong args,
                             uint32_t nret, target_ulong rets);

#define SPAPR_PCI_BUID          0x800000020000001ULL
#define SPAPR_PCI_MEM_WIN_ADDR  (KVM_MMIO_START + 0xA0000000)
#define SPAPR_PCI_MEM_WIN_SIZE  0x20000000
#define SPAPR_PCI_IO_WIN_ADDR   (SPAPR_PCI_MEM_WIN_ADDR + SPAPR_PCI_MEM_WIN_SIZE)
#define SPAPR_PCI_IO_WIN_SIZE	0x2000000

#define SPAPR_PCI_WIN_START	SPAPR_PCI_MEM_WIN_ADDR
#define SPAPR_PCI_WIN_END	(SPAPR_PCI_IO_WIN_ADDR + SPAPR_PCI_IO_WIN_SIZE)

#endif /* !defined (__HW_SPAPR_H__) */
