/*
 * PPC64 cpu-specific definitions
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

/* Architecture-specific kvm_cpu definitions. */

#include <linux/kvm.h>	/* for struct kvm_regs */
#include <stdbool.h>
#include <pthread.h>

#define MSR_SF		(1ULL<<63)
#define MSR_HV		(1ULL<<60)
#define MSR_VEC		(1ULL<<25)
#define MSR_VSX		(1ULL<<23)
#define MSR_POW		(1ULL<<18)
#define MSR_EE		(1ULL<<15)
#define MSR_PR		(1ULL<<14)
#define MSR_FP		(1ULL<<13)
#define MSR_ME		(1ULL<<12)
#define MSR_FE0		(1ULL<<11)
#define MSR_SE		(1ULL<<10)
#define MSR_BE		(1ULL<<9)
#define MSR_FE1		(1ULL<<8)
#define MSR_IR		(1ULL<<5)
#define MSR_DR		(1ULL<<4)
#define MSR_PMM		(1ULL<<2)
#define MSR_RI		(1ULL<<1)
#define MSR_LE		(1ULL<<0)

#define POWER7_EXT_IRQ	0

struct kvm;

struct kvm_cpu {
	pthread_t		thread;		/* VCPU thread */

	unsigned long		cpu_id;

	struct kvm		*kvm;		/* parent KVM */
	int			vcpu_fd;	/* For VCPU ioctls() */
	struct kvm_run		*kvm_run;

	struct kvm_regs		regs;
	struct kvm_sregs	sregs;
	struct kvm_fpu		fpu;

	u8			is_running;
	u8			paused;
	u8			needs_nmi;
	/*
	 * Although PPC KVM doesn't yet support coalesced MMIO, generic code
	 * needs this in our kvm_cpu:
	 */
	struct kvm_coalesced_mmio_ring  *ring;
};

void kvm_cpu__irq(struct kvm_cpu *vcpu, int pin, int level);

/* This is never actually called on PPC. */
static inline bool kvm_cpu__emulate_io(struct kvm_cpu *vcpu, u16 port, void *data, int direction, int size, u32 count)
{
	return false;
}

bool kvm_cpu__emulate_mmio(struct kvm_cpu *vcpu, u64 phys_addr, u8 *data, u32 len, u8 is_write);

#endif /* KVM__KVM_CPU_ARCH_H */
