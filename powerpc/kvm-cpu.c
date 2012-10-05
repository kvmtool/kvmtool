/*
 * PPC64 processor support
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/kvm-cpu.h"

#include "kvm/symbol.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include "spapr.h"
#include "spapr_pci.h"
#include "xics.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>

static int debug_fd;

void kvm_cpu__set_debug_fd(int fd)
{
	debug_fd = fd;
}

int kvm_cpu__get_debug_fd(void)
{
	return debug_fd;
}

static struct kvm_cpu *kvm_cpu__new(struct kvm *kvm)
{
	struct kvm_cpu *vcpu;

	vcpu		= calloc(1, sizeof *vcpu);
	if (!vcpu)
		return NULL;

	vcpu->kvm	= kvm;

	return vcpu;
}

void kvm_cpu__delete(struct kvm_cpu *vcpu)
{
	free(vcpu);
}

struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id)
{
	struct kvm_cpu *vcpu;
	int mmap_size;
	struct kvm_enable_cap papr_cap = { .cap = KVM_CAP_PPC_PAPR };

	vcpu		= kvm_cpu__new(kvm);
	if (!vcpu)
		return NULL;

	vcpu->cpu_id	= cpu_id;

	vcpu->vcpu_fd = ioctl(vcpu->kvm->vm_fd, KVM_CREATE_VCPU, cpu_id);
	if (vcpu->vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU ioctl");

	mmap_size = ioctl(vcpu->kvm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

	vcpu->kvm_run = mmap(NULL, mmap_size, PROT_RW, MAP_SHARED, vcpu->vcpu_fd, 0);
	if (vcpu->kvm_run == MAP_FAILED)
		die("unable to mmap vcpu fd");

	if (ioctl(vcpu->vcpu_fd, KVM_ENABLE_CAP, &papr_cap) < 0)
		die("unable to enable PAPR capability");

	/*
	 * We start all CPUs, directing non-primary threads into the kernel's
	 * secondary start point.  When we come to support SLOF, we will start
	 * only one and SLOF will RTAS call us to ask for others to be
	 * started.  (FIXME: make more generic & interface with whichever
	 * firmware a platform may be using.)
	 */
	vcpu->is_running = true;

	return vcpu;
}

static void kvm_cpu__setup_fpu(struct kvm_cpu *vcpu)
{
	/* Don't have to do anything, there's no expected FPU state. */
}

static void kvm_cpu__setup_regs(struct kvm_cpu *vcpu)
{
	/*
	 * FIXME: This assumes PPC64 and Linux guest.  It doesn't use the
	 * OpenFirmware entry method, but instead the "embedded" entry which
	 * passes the FDT address directly.
	 */
	struct kvm_regs *r = &vcpu->regs;

	if (vcpu->cpu_id == 0) {
		r->pc = KERNEL_START_ADDR;
		r->gpr[3] = vcpu->kvm->arch.fdt_gra;
		r->gpr[5] = 0;
	} else {
		r->pc = KERNEL_SECONDARY_START_ADDR;
		r->gpr[3] = vcpu->cpu_id;
	}
	r->msr = 0x8000000000001000UL; /* 64bit, non-HV, ME */

	if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &vcpu->regs) < 0)
		die_perror("KVM_SET_REGS failed");
}

static void kvm_cpu__setup_sregs(struct kvm_cpu *vcpu)
{
	/*
	 * Some sregs setup to initialise SDR1/PVR/HIOR on PPC64 SPAPR
	 * platforms using PR KVM.  (Technically, this is all ignored on
	 * SPAPR HV KVM.)  Different setup is required for non-PV non-SPAPR
	 * platforms!  (FIXME.)
	 */
	struct kvm_sregs sregs;
	struct kvm_one_reg reg = {};
	u64 value;

	if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
		die("KVM_GET_SREGS failed");

	sregs.u.s.sdr1 = vcpu->kvm->arch.sdr1;
	sregs.pvr = vcpu->kvm->arch.pvr;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
		die("KVM_SET_SREGS failed");

	reg.id = KVM_REG_PPC_HIOR;
	value = 0;
	reg.addr = (u64)&value;
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die("KVM_SET_ONE_REG failed");
}

/**
 * kvm_cpu__reset_vcpu - reset virtual CPU to a known state
 */
void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu)
{
	kvm_cpu__setup_regs(vcpu);
	kvm_cpu__setup_sregs(vcpu);
	kvm_cpu__setup_fpu(vcpu);
}

/* kvm_cpu__irq - set KVM's IRQ flag on this vcpu */
void kvm_cpu__irq(struct kvm_cpu *vcpu, int pin, int level)
{
	unsigned int virq = level ? KVM_INTERRUPT_SET_LEVEL : KVM_INTERRUPT_UNSET;

	/* FIXME: POWER-specific */
	if (pin != POWER7_EXT_IRQ)
		return;
	if (ioctl(vcpu->vcpu_fd, KVM_INTERRUPT, &virq) < 0)
		pr_warning("Could not KVM_INTERRUPT.");
}

void kvm_cpu__arch_nmi(struct kvm_cpu *cpu)
{
}

bool kvm_cpu__handle_exit(struct kvm_cpu *vcpu)
{
	bool ret = true;
	struct kvm_run *run = vcpu->kvm_run;
	switch(run->exit_reason) {
	case KVM_EXIT_PAPR_HCALL:
		run->papr_hcall.ret = spapr_hypercall(vcpu, run->papr_hcall.nr,
						      (target_ulong*)run->papr_hcall.args);
		break;
	default:
		ret = false;
	}
	return ret;
}

bool kvm_cpu__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write)
{
	/*
	 * FIXME: This function will need to be split in order to support
	 * various PowerPC platforms/PHB types, etc.  It currently assumes SPAPR
	 * PPC64 guest.
	 */
	bool ret = false;

	if ((phys_addr >= SPAPR_PCI_WIN_START) &&
	    (phys_addr < SPAPR_PCI_WIN_END)) {
		ret = spapr_phb_mmio(kvm, phys_addr, data, len, is_write);
	} else {
		pr_warning("MMIO %s unknown address %llx (size %d)!\n",
			   is_write ? "write to" : "read from",
			   phys_addr, len);
	}
	return ret;
}

#define CONDSTR_BIT(m, b) (((m) & MSR_##b) ? #b" " : "")

void kvm_cpu__show_registers(struct kvm_cpu *vcpu)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	int r;

	if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
		die("KVM_GET_REGS failed");
        if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
		die("KVM_GET_SREGS failed");

	dprintf(debug_fd, "\n Registers:\n");
	dprintf(debug_fd, " NIP:   %016llx  MSR:   %016llx "
		"( %s%s%s%s%s%s%s%s%s%s%s%s)\n",
		regs.pc, regs.msr,
		CONDSTR_BIT(regs.msr, SF),
		CONDSTR_BIT(regs.msr, HV), /* ! */
		CONDSTR_BIT(regs.msr, VEC),
		CONDSTR_BIT(regs.msr, VSX),
		CONDSTR_BIT(regs.msr, EE),
		CONDSTR_BIT(regs.msr, PR),
		CONDSTR_BIT(regs.msr, FP),
		CONDSTR_BIT(regs.msr, ME),
		CONDSTR_BIT(regs.msr, IR),
		CONDSTR_BIT(regs.msr, DR),
		CONDSTR_BIT(regs.msr, RI),
		CONDSTR_BIT(regs.msr, LE));
	dprintf(debug_fd, " CTR:   %016llx  LR:    %016llx  CR:   %08llx\n",
		regs.ctr, regs.lr, regs.cr);
	dprintf(debug_fd, " SRR0:  %016llx  SRR1:  %016llx  XER:  %016llx\n",
		regs.srr0, regs.srr1, regs.xer);
	dprintf(debug_fd, " SPRG0: %016llx  SPRG1: %016llx\n",
		regs.sprg0, regs.sprg1);
	dprintf(debug_fd, " SPRG2: %016llx  SPRG3: %016llx\n",
		regs.sprg2, regs.sprg3);
	dprintf(debug_fd, " SPRG4: %016llx  SPRG5: %016llx\n",
		regs.sprg4, regs.sprg5);
	dprintf(debug_fd, " SPRG6: %016llx  SPRG7: %016llx\n",
		regs.sprg6, regs.sprg7);
	dprintf(debug_fd, " GPRs:\n ");
	for (r = 0; r < 32; r++) {
		dprintf(debug_fd, "%016llx  ", regs.gpr[r]);
		if ((r & 3) == 3)
			dprintf(debug_fd, "\n ");
	}
	dprintf(debug_fd, "\n");

	/* FIXME: Assumes SLB-based (book3s) guest */
	for (r = 0; r < 32; r++) {
		dprintf(debug_fd, " SLB%02d  %016llx %016llx\n", r,
			sregs.u.s.ppc64.slb[r].slbe,
			sregs.u.s.ppc64.slb[r].slbv);
	}
	dprintf(debug_fd, "----------\n");
}

void kvm_cpu__show_code(struct kvm_cpu *vcpu)
{
	if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &vcpu->regs) < 0)
		die("KVM_GET_REGS failed");

	/* FIXME: Dump/disassemble some code...! */

	dprintf(debug_fd, "\n Stack:\n");
	dprintf(debug_fd,   " ------\n");
	/* Only works in real mode: */
	kvm__dump_mem(vcpu->kvm, vcpu->regs.gpr[1], 32);
}

void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu)
{
	/* Does nothing yet */
}
