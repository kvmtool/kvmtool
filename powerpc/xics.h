/*
 * PAPR Virtualized Interrupt System, aka ICS/ICP aka xics
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef XICS_H
#define XICS_H

#define XICS_IPI        0x2

struct kvm_cpu;
struct icp_state;

struct icp_state *xics_system_init(unsigned int nr_irqs, unsigned int nr_cpus);
void xics_cpu_register(struct kvm_cpu *vcpu);
int xics_alloc_irqnum(void);

#endif
