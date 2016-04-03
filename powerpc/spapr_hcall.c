/*
 * SPAPR hypercalls
 *
 * Borrowed heavily from QEMU's spapr_hcall.c,
 * Copyright (c) 2010 David Gibson, IBM Corporation.
 *
 * Copyright (c) 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "spapr.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"

#include <stdio.h>
#include <assert.h>
#include <sys/eventfd.h>

static spapr_hcall_fn papr_hypercall_table[(MAX_HCALL_OPCODE / 4) + 1];
static spapr_hcall_fn kvmppc_hypercall_table[KVMPPC_HCALL_MAX -
					     KVMPPC_HCALL_BASE + 1];

static target_ulong h_set_dabr(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	/* FIXME:  Implement this for -PR.  (-HV does this in kernel.) */
	return H_HARDWARE;
}

static target_ulong h_rtas(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	target_ulong rtas_r3 = args[0];
	/*
	 * Pointer read from phys mem; these ptrs cannot be MMIO (!) so just
	 * reference guest RAM directly.
	 */
	uint32_t token, nargs, nret;

	token = rtas_ld(vcpu->kvm, rtas_r3, 0);
	nargs = rtas_ld(vcpu->kvm, rtas_r3, 1);
	nret  = rtas_ld(vcpu->kvm, rtas_r3, 2);

	return spapr_rtas_call(vcpu, token, nargs, rtas_r3 + 12,
			       nret, rtas_r3 + 12 + 4*nargs);
}

static target_ulong h_logical_load(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	/* SLOF will require these, though kernel doesn't. */
	die(__PRETTY_FUNCTION__);
	return H_PARAMETER;
}

static target_ulong h_logical_store(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	/* SLOF will require these, though kernel doesn't. */
	die(__PRETTY_FUNCTION__);
	return H_PARAMETER;
}

static target_ulong h_logical_icbi(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	/* KVM will trap this in the kernel.  Die if it misses. */
	die(__PRETTY_FUNCTION__);
	return H_SUCCESS;
}

static target_ulong h_logical_dcbf(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	/* KVM will trap this in the kernel.  Die if it misses. */
	die(__PRETTY_FUNCTION__);
	return H_SUCCESS;
}

struct lpcr_data {
	struct kvm_cpu	*cpu;
	int		mode;
};

static void get_cpu_lpcr(struct kvm_cpu *vcpu, target_ulong *lpcr)
{
	struct kvm_one_reg reg = {
		.id = KVM_REG_PPC_LPCR_64,
		.addr = (__u64)lpcr
	};

	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg))
		die("Couldn't read vcpu reg?!");
}

static void set_cpu_lpcr(struct kvm_cpu *vcpu, target_ulong *lpcr)
{
	struct kvm_one_reg reg = {
		.id = KVM_REG_PPC_LPCR_64,
		.addr = (__u64)lpcr
	};

	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg))
		die("Couldn't write vcpu reg?!");
}

static void set_endian_task(struct kvm_cpu *vcpu, void *data)
{
	target_ulong mflags = (target_ulong)data;
	target_ulong lpcr;

	get_cpu_lpcr(vcpu, &lpcr);

	if (mflags == H_SET_MODE_ENDIAN_BIG)
		lpcr &= ~LPCR_ILE;
	else
		lpcr |= LPCR_ILE;

	set_cpu_lpcr(vcpu, &lpcr);
}

static target_ulong h_set_mode(struct kvm_cpu *vcpu, target_ulong opcode, target_ulong *args)
{
	int ret;

	switch (args[1]) {
	case H_SET_MODE_RESOURCE_LE: {
		struct kvm_cpu_task task;
		task.func = set_endian_task;
		task.data = (void *)args[0];
		kvm_cpu__run_on_all_cpus(vcpu->kvm, &task);
		ret = H_SUCCESS;
		break;
	}
	default:
		ret = H_FUNCTION;
		break;
	}

	return ret;
}


void spapr_register_hypercall(target_ulong opcode, spapr_hcall_fn fn)
{
	spapr_hcall_fn *slot;

	if (opcode <= MAX_HCALL_OPCODE) {
		assert((opcode & 0x3) == 0);

		slot = &papr_hypercall_table[opcode / 4];
	} else {
		assert((opcode >= KVMPPC_HCALL_BASE) &&
		       (opcode <= KVMPPC_HCALL_MAX));

		slot = &kvmppc_hypercall_table[opcode - KVMPPC_HCALL_BASE];
	}

	assert(!(*slot) || (fn == *slot));
	*slot = fn;
}

target_ulong spapr_hypercall(struct kvm_cpu *vcpu, target_ulong opcode,
			     target_ulong *args)
{
	if ((opcode <= MAX_HCALL_OPCODE)
	    && ((opcode & 0x3) == 0)) {
		spapr_hcall_fn fn = papr_hypercall_table[opcode / 4];

		if (fn) {
			return fn(vcpu, opcode, args);
		}
	} else if ((opcode >= KVMPPC_HCALL_BASE) &&
		   (opcode <= KVMPPC_HCALL_MAX)) {
		spapr_hcall_fn fn = kvmppc_hypercall_table[opcode -
							   KVMPPC_HCALL_BASE];

		if (fn) {
			return fn(vcpu, opcode, args);
		}
	}

	hcall_dprintf("Unimplemented hcall 0x%lx\n", opcode);
	return H_FUNCTION;
}

void hypercall_init(void)
{
	/* hcall-dabr */
	spapr_register_hypercall(H_SET_DABR, h_set_dabr);

	spapr_register_hypercall(H_LOGICAL_CI_LOAD, h_logical_load);
	spapr_register_hypercall(H_LOGICAL_CI_STORE, h_logical_store);
	spapr_register_hypercall(H_LOGICAL_CACHE_LOAD, h_logical_load);
	spapr_register_hypercall(H_LOGICAL_CACHE_STORE, h_logical_store);
	spapr_register_hypercall(H_LOGICAL_ICBI, h_logical_icbi);
	spapr_register_hypercall(H_LOGICAL_DCBF, h_logical_dcbf);
	spapr_register_hypercall(H_SET_MODE, h_set_mode);

	/* KVM-PPC specific hcalls */
	spapr_register_hypercall(KVMPPC_H_RTAS, h_rtas);
}
