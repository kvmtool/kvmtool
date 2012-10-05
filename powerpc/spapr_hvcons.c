/*
 * SPAPR HV console
 *
 * Borrowed lightly from QEMU's spapr_vty.c, Copyright (c) 2010 David Gibson,
 * IBM Corporation.
 *
 * Copyright (c) 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/term.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"
#include "spapr.h"
#include "spapr_hvcons.h"

#include <stdio.h>
#include <sys/uio.h>
#include <errno.h>

#include <linux/byteorder.h>

union hv_chario {
	struct {
		uint64_t char0_7;
		uint64_t char8_15;
	} a;
	uint8_t buf[16];
};

static unsigned long h_put_term_char(struct kvm_cpu *vcpu, unsigned long opcode, unsigned long *args)
{
	/* To do: Read register from args[0], and check it. */
	unsigned long len = args[1];
	union hv_chario data;
	struct iovec iov;

	if (len > 16) {
		return H_PARAMETER;
	}
	data.a.char0_7 = cpu_to_be64(args[2]);
	data.a.char8_15 = cpu_to_be64(args[3]);

	iov.iov_base = data.buf;
	iov.iov_len = len;
	do {
		int ret;

		if (vcpu->kvm->cfg.active_console == CONSOLE_HV)
			ret = term_putc_iov(&iov, 1, 0);
		else
			ret = 0;
		if (ret < 0) {
			die("term_putc_iov error %d!\n", errno);
		}
		iov.iov_base += ret;
		iov.iov_len -= ret;
	} while (iov.iov_len > 0);

	return H_SUCCESS;
}


static unsigned long h_get_term_char(struct kvm_cpu *vcpu, unsigned long opcode, unsigned long *args)
{
	/* To do: Read register from args[0], and check it. */
	unsigned long *len = args + 0;
	unsigned long *char0_7 = args + 1;
	unsigned long *char8_15 = args + 2;
	union hv_chario data;
	struct iovec iov;

	if (vcpu->kvm->cfg.active_console != CONSOLE_HV)
		return H_SUCCESS;

	if (term_readable(0)) {
		iov.iov_base = data.buf;
		iov.iov_len = 16;

		*len = term_getc_iov(vcpu->kvm, &iov, 1, 0);
		*char0_7 = be64_to_cpu(data.a.char0_7);
		*char8_15 = be64_to_cpu(data.a.char8_15);
	} else {
		*len = 0;
	}

	return H_SUCCESS;
}

void spapr_hvcons_poll(struct kvm *kvm)
{
	if (term_readable(0)) {
		/*
		 * We can inject an IRQ to guest here if we want.  The guest
		 * will happily poll, though, so not required.
		 */
	}
}

void spapr_hvcons_init(void)
{
	spapr_register_hypercall(H_PUT_TERM_CHAR, h_put_term_char);
	spapr_register_hypercall(H_GET_TERM_CHAR, h_get_term_char);
}
