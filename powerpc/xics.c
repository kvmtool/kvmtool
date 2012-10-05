/*
 * PAPR Virtualized Interrupt System, aka ICS/ICP aka xics
 *
 * Borrowed heavily from QEMU's xics.c,
 * Copyright (c) 2010,2011 David Gibson, IBM Corporation.
 *
 * Modifications copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "spapr.h"
#include "xics.h"
#include "kvm/util.h"

#include <stdio.h>
#include <malloc.h>

#define XICS_NUM_IRQS	1024


/* #define DEBUG_XICS yes */
#ifdef DEBUG_XICS
#define xics_dprintf(fmt, ...)					\
	do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define xics_dprintf(fmt, ...)			\
	do { } while (0)
#endif

/*
 * ICP: Presentation layer
 */

struct icp_server_state {
	uint32_t xirr;
	uint8_t pending_priority;
	uint8_t mfrr;
	struct kvm_cpu *cpu;
};

#define XICS_IRQ_OFFSET 16
#define XISR_MASK	0x00ffffff
#define CPPR_MASK	0xff000000

#define XISR(ss)   (((ss)->xirr) & XISR_MASK)
#define CPPR(ss)   (((ss)->xirr) >> 24)

struct ics_state;

struct icp_state {
	unsigned long nr_servers;
	struct icp_server_state *ss;
	struct ics_state *ics;
};

static void ics_reject(struct ics_state *ics, int nr);
static void ics_resend(struct ics_state *ics);
static void ics_eoi(struct ics_state *ics, int nr);

static inline void cpu_irq_raise(struct kvm_cpu *vcpu)
{
	xics_dprintf("INT1[%p]\n", vcpu);
	kvm_cpu__irq(vcpu, POWER7_EXT_IRQ, 1);
}

static inline void cpu_irq_lower(struct kvm_cpu *vcpu)
{
	xics_dprintf("INT0[%p]\n", vcpu);
	kvm_cpu__irq(vcpu, POWER7_EXT_IRQ, 0);
}

static void icp_check_ipi(struct icp_state *icp, int server)
{
	struct icp_server_state *ss = icp->ss + server;

	if (XISR(ss) && (ss->pending_priority <= ss->mfrr)) {
		return;
	}

	if (XISR(ss)) {
		ics_reject(icp->ics, XISR(ss));
	}

	ss->xirr = (ss->xirr & ~XISR_MASK) | XICS_IPI;
	ss->pending_priority = ss->mfrr;
	cpu_irq_raise(ss->cpu);
}

static void icp_resend(struct icp_state *icp, int server)
{
	struct icp_server_state *ss = icp->ss + server;

	if (ss->mfrr < CPPR(ss)) {
		icp_check_ipi(icp, server);
	}
	ics_resend(icp->ics);
}

static void icp_set_cppr(struct icp_state *icp, int server, uint8_t cppr)
{
	struct icp_server_state *ss = icp->ss + server;
	uint8_t old_cppr;
	uint32_t old_xisr;

	old_cppr = CPPR(ss);
	ss->xirr = (ss->xirr & ~CPPR_MASK) | (cppr << 24);

	if (cppr < old_cppr) {
		if (XISR(ss) && (cppr <= ss->pending_priority)) {
			old_xisr = XISR(ss);
			ss->xirr &= ~XISR_MASK; /* Clear XISR */
			cpu_irq_lower(ss->cpu);
			ics_reject(icp->ics, old_xisr);
		}
	} else {
		if (!XISR(ss)) {
			icp_resend(icp, server);
		}
	}
}

static void icp_set_mfrr(struct icp_state *icp, int nr, uint8_t mfrr)
{
	struct icp_server_state *ss = icp->ss + nr;

	ss->mfrr = mfrr;
	if (mfrr < CPPR(ss)) {
		icp_check_ipi(icp, nr);
	}
}

static uint32_t icp_accept(struct icp_server_state *ss)
{
	uint32_t xirr;

	cpu_irq_lower(ss->cpu);
	xirr = ss->xirr;
	ss->xirr = ss->pending_priority << 24;
	return xirr;
}

static void icp_eoi(struct icp_state *icp, int server, uint32_t xirr)
{
	struct icp_server_state *ss = icp->ss + server;

	ics_eoi(icp->ics, xirr & XISR_MASK);
	/* Send EOI -> ICS */
	ss->xirr = (ss->xirr & ~CPPR_MASK) | (xirr & CPPR_MASK);
	if (!XISR(ss)) {
		icp_resend(icp, server);
	}
}

static void icp_irq(struct icp_state *icp, int server, int nr, uint8_t priority)
{
	struct icp_server_state *ss = icp->ss + server;
	xics_dprintf("icp_irq(nr %d, server %d, prio 0x%x)\n", nr, server, priority);
	if ((priority >= CPPR(ss))
	    || (XISR(ss) && (ss->pending_priority <= priority))) {
		xics_dprintf("reject %d, CPPR 0x%x, XISR 0x%x, pprio 0x%x, prio 0x%x\n",
			     nr, CPPR(ss), XISR(ss), ss->pending_priority, priority);
		ics_reject(icp->ics, nr);
	} else {
		if (XISR(ss)) {
			xics_dprintf("reject %d, CPPR 0x%x, XISR 0x%x, pprio 0x%x, prio 0x%x\n",
				     nr, CPPR(ss), XISR(ss), ss->pending_priority, priority);
			ics_reject(icp->ics, XISR(ss));
		}
		ss->xirr = (ss->xirr & ~XISR_MASK) | (nr & XISR_MASK);
		ss->pending_priority = priority;
		cpu_irq_raise(ss->cpu);
	}
}

/*
 * ICS: Source layer
 */

struct ics_irq_state {
	int server;
	uint8_t priority;
	uint8_t saved_priority;
	int rejected:1;
	int masked_pending:1;
};

struct ics_state {
	unsigned int nr_irqs;
	unsigned int offset;
	struct ics_irq_state *irqs;
	struct icp_state *icp;
};

static int ics_valid_irq(struct ics_state *ics, uint32_t nr)
{
	return (nr >= ics->offset)
		&& (nr < (ics->offset + ics->nr_irqs));
}

static void ics_set_irq_msi(struct ics_state *ics, int srcno, int val)
{
	struct ics_irq_state *irq = ics->irqs + srcno;

	if (val) {
		if (irq->priority == 0xff) {
			xics_dprintf(" irq pri ff, masked pending\n");
			irq->masked_pending = 1;
		} else	{
			icp_irq(ics->icp, irq->server, srcno + ics->offset, irq->priority);
		}
	}
}

static void ics_reject_msi(struct ics_state *ics, int nr)
{
	struct ics_irq_state *irq = ics->irqs + nr - ics->offset;

	irq->rejected = 1;
}

static void ics_resend_msi(struct ics_state *ics)
{
	unsigned int i;

	for (i = 0; i < ics->nr_irqs; i++) {
		struct ics_irq_state *irq = ics->irqs + i;

		/* FIXME: filter by server#? */
		if (irq->rejected) {
			irq->rejected = 0;
			if (irq->priority != 0xff) {
				icp_irq(ics->icp, irq->server, i + ics->offset, irq->priority);
			}
		}
	}
}

static void ics_write_xive_msi(struct ics_state *ics, int nr, int server,
			       uint8_t priority)
{
	struct ics_irq_state *irq = ics->irqs + nr - ics->offset;

	irq->server = server;
	irq->priority = priority;
	xics_dprintf("ics_write_xive_msi(nr %d, server %d, pri 0x%x)\n", nr, server, priority);

	if (!irq->masked_pending || (priority == 0xff)) {
		return;
	}

	irq->masked_pending = 0;
	icp_irq(ics->icp, server, nr, priority);
}

static void ics_reject(struct ics_state *ics, int nr)
{
	ics_reject_msi(ics, nr);
}

static void ics_resend(struct ics_state *ics)
{
	ics_resend_msi(ics);
}

static void ics_eoi(struct ics_state *ics, int nr)
{
}

/*
 * Exported functions
 */

static int allocated_irqnum = XICS_IRQ_OFFSET;

/*
 * xics_alloc_irqnum(): This is hacky.  The problem boils down to the PCI device
 * code which just calls kvm__irq_line( .. pcidev->pci_hdr.irq_line ..) at will.
 * Each PCI device's IRQ line is allocated by irq__register_device() (which
 * allocates an IRQ AND allocates a.. PCI device num..).
 *
 * In future I'd like to at least mimic some kind of 'upstream IRQ controller'
 * whereby PCI devices let their PHB know when they want to IRQ, and that
 * percolates up.
 *
 * For now, allocate a REAL xics irq number and (via irq__register_device) push
 * that into the config space.	8 bits only though!
 */
int xics_alloc_irqnum(void)
{
	int irq = allocated_irqnum++;

	if (irq > 255)
		die("Huge numbers of IRQs aren't supported with the daft kvmtool IRQ system.");

	return irq;
}

static target_ulong h_cppr(struct kvm_cpu *vcpu,
			   target_ulong opcode, target_ulong *args)
{
	target_ulong cppr = args[0];

	xics_dprintf("h_cppr(%lx)\n", cppr);
	icp_set_cppr(vcpu->kvm->arch.icp, vcpu->cpu_id, cppr);
	return H_SUCCESS;
}

static target_ulong h_ipi(struct kvm_cpu *vcpu,
			  target_ulong opcode, target_ulong *args)
{
	target_ulong server = args[0];
	target_ulong mfrr = args[1];

	xics_dprintf("h_ipi(%lx, %lx)\n", server, mfrr);
	if (server >= vcpu->kvm->arch.icp->nr_servers) {
		return H_PARAMETER;
	}

	icp_set_mfrr(vcpu->kvm->arch.icp, server, mfrr);
	return H_SUCCESS;
}

static target_ulong h_xirr(struct kvm_cpu *vcpu,
			   target_ulong opcode, target_ulong *args)
{
	uint32_t xirr = icp_accept(vcpu->kvm->arch.icp->ss + vcpu->cpu_id);

	xics_dprintf("h_xirr() = %x\n", xirr);
	args[0] = xirr;
	return H_SUCCESS;
}

static target_ulong h_eoi(struct kvm_cpu *vcpu,
			  target_ulong opcode, target_ulong *args)
{
	target_ulong xirr = args[0];

	xics_dprintf("h_eoi(%lx)\n", xirr);
	icp_eoi(vcpu->kvm->arch.icp, vcpu->cpu_id, xirr);
	return H_SUCCESS;
}

static void rtas_set_xive(struct kvm_cpu *vcpu, uint32_t token,
			  uint32_t nargs, target_ulong args,
			  uint32_t nret, target_ulong rets)
{
	struct ics_state *ics = vcpu->kvm->arch.icp->ics;
	uint32_t nr, server, priority;

	if ((nargs != 3) || (nret != 1)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	nr = rtas_ld(vcpu->kvm, args, 0);
	server = rtas_ld(vcpu->kvm, args, 1);
	priority = rtas_ld(vcpu->kvm, args, 2);

	xics_dprintf("rtas_set_xive(%x,%x,%x)\n", nr, server, priority);
	if (!ics_valid_irq(ics, nr) || (server >= ics->icp->nr_servers)
	    || (priority > 0xff)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	ics_write_xive_msi(ics, nr, server, priority);

	rtas_st(vcpu->kvm, rets, 0, 0); /* Success */
}

static void rtas_get_xive(struct kvm_cpu *vcpu, uint32_t token,
			  uint32_t nargs, target_ulong args,
			  uint32_t nret, target_ulong rets)
{
	struct ics_state *ics = vcpu->kvm->arch.icp->ics;
	uint32_t nr;

	if ((nargs != 1) || (nret != 3)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	nr = rtas_ld(vcpu->kvm, args, 0);

	if (!ics_valid_irq(ics, nr)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	rtas_st(vcpu->kvm, rets, 0, 0); /* Success */
	rtas_st(vcpu->kvm, rets, 1, ics->irqs[nr - ics->offset].server);
	rtas_st(vcpu->kvm, rets, 2, ics->irqs[nr - ics->offset].priority);
}

static void rtas_int_off(struct kvm_cpu *vcpu, uint32_t token,
			 uint32_t nargs, target_ulong args,
			 uint32_t nret, target_ulong rets)
{
	struct ics_state *ics = vcpu->kvm->arch.icp->ics;
	uint32_t nr;

	if ((nargs != 1) || (nret != 1)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	nr = rtas_ld(vcpu->kvm, args, 0);

	if (!ics_valid_irq(ics, nr)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	/* ME: QEMU wrote xive_msi here, in #if 0.  Deleted. */

	rtas_st(vcpu->kvm, rets, 0, 0); /* Success */
}

static void rtas_int_on(struct kvm_cpu *vcpu, uint32_t token,
			uint32_t nargs, target_ulong args,
			uint32_t nret, target_ulong rets)
{
	struct ics_state *ics = vcpu->kvm->arch.icp->ics;
	uint32_t nr;

	if ((nargs != 1) || (nret != 1)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	nr = rtas_ld(vcpu->kvm, args, 0);

	if (!ics_valid_irq(ics, nr)) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	/* ME: QEMU wrote xive_msi here, in #if 0.  Deleted. */

	rtas_st(vcpu->kvm, rets, 0, 0); /* Success */
}

static int xics_init(struct kvm *kvm)
{
	int max_server_num;
	unsigned int i;
	struct icp_state *icp;
	struct ics_state *ics;
	int j;

	max_server_num = kvm->nrcpus;

	icp = malloc(sizeof(*icp));
	icp->nr_servers = max_server_num + 1;
	icp->ss = malloc(icp->nr_servers * sizeof(struct icp_server_state));

	for (i = 0; i < icp->nr_servers; i++) {
		icp->ss[i].xirr = 0;
		icp->ss[i].pending_priority = 0;
		icp->ss[i].cpu = 0;
		icp->ss[i].mfrr = 0xff;
	}

	/*
	 * icp->ss[env->cpu_index].cpu is set by CPUs calling in to
	 * xics_cpu_register().
	 */

	ics = malloc(sizeof(*ics));
	ics->nr_irqs = XICS_NUM_IRQS;
	ics->offset = XICS_IRQ_OFFSET;
	ics->irqs = malloc(ics->nr_irqs * sizeof(struct ics_irq_state));

	icp->ics = ics;
	ics->icp = icp;

	for (i = 0; i < ics->nr_irqs; i++) {
		ics->irqs[i].server = 0;
		ics->irqs[i].priority = 0xff;
		ics->irqs[i].saved_priority = 0xff;
		ics->irqs[i].rejected = 0;
		ics->irqs[i].masked_pending = 0;
	}

	spapr_register_hypercall(H_CPPR, h_cppr);
	spapr_register_hypercall(H_IPI, h_ipi);
	spapr_register_hypercall(H_XIRR, h_xirr);
	spapr_register_hypercall(H_EOI, h_eoi);

	spapr_rtas_register("ibm,set-xive", rtas_set_xive);
	spapr_rtas_register("ibm,get-xive", rtas_get_xive);
	spapr_rtas_register("ibm,int-off", rtas_int_off);
	spapr_rtas_register("ibm,int-on", rtas_int_on);

	for (j = 0; j < kvm->nrcpus; j++) {
		struct kvm_cpu *vcpu = kvm->cpus[j];

		if (vcpu->cpu_id >= icp->nr_servers)
			die("Invalid server number for cpuid %ld\n", vcpu->cpu_id);

		icp->ss[vcpu->cpu_id].cpu = vcpu;
	}

	kvm->arch.icp = icp;

	return 0;
}
base_init(xics_init);


void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	/*
	 * Route event to ICS, which routes to ICP, which eventually does a
	 * kvm_cpu__irq(vcpu, POWER7_EXT_IRQ, 1)
	 */
	xics_dprintf("Raising IRQ %d -> %d\n", irq, level);
	ics_set_irq_msi(kvm->arch.icp->ics, irq - kvm->arch.icp->ics->offset, level);
}
