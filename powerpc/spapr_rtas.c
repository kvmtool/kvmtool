/*
 * SPAPR base RTAS calls
 *
 * Borrowed heavily from QEMU's spapr_rtas.c
 * Copyright (c) 2010-2011 David Gibson, IBM Corporation.
 *
 * Modifications copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "libfdt.h"

#include "spapr.h"

#include <stdio.h>
#include <assert.h>

#define TOKEN_BASE      0x2000
#define TOKEN_MAX       0x100

#define RTAS_CONSOLE

static struct rtas_call {
	const char *name;
	spapr_rtas_fn fn;
} rtas_table[TOKEN_MAX];

struct rtas_call *rtas_next = rtas_table;


static void rtas_display_character(struct kvm_cpu *vcpu,
                                   uint32_t token, uint32_t nargs,
                                   target_ulong args,
                                   uint32_t nret, target_ulong rets)
{
	char c = rtas_ld(vcpu->kvm, args, 0);
	term_putc(&c, 1, 0);
	rtas_st(vcpu->kvm, rets, 0, 0);
}

#ifdef RTAS_CONSOLE
static void rtas_put_term_char(struct kvm_cpu *vcpu,
			       uint32_t token, uint32_t nargs,
			       target_ulong args,
			       uint32_t nret, target_ulong rets)
{
	char c = rtas_ld(vcpu->kvm, args, 0);

	if (vcpu->kvm->cfg.active_console == CONSOLE_HV)
		term_putc(&c, 1, 0);

	rtas_st(vcpu->kvm, rets, 0, 0);
}

static void rtas_get_term_char(struct kvm_cpu *vcpu,
			       uint32_t token, uint32_t nargs,
			       target_ulong args,
			       uint32_t nret, target_ulong rets)
{
	int c;

	if (vcpu->kvm->cfg.active_console == CONSOLE_HV && term_readable(0) &&
	    (c = term_getc(vcpu->kvm, 0)) >= 0) {
		rtas_st(vcpu->kvm, rets, 0, 0);
		rtas_st(vcpu->kvm, rets, 1, c);
	} else {
		rtas_st(vcpu->kvm, rets, 0, -2);
	}
}
#endif

static void rtas_get_time_of_day(struct kvm_cpu *vcpu,
                                 uint32_t token, uint32_t nargs,
                                 target_ulong args,
                                 uint32_t nret, target_ulong rets)
{
	struct tm tm;
	time_t tnow;

	if (nret != 8) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	tnow = time(NULL);
	/* Guest time is currently not offset in any way. */
	gmtime_r(&tnow, &tm);

	rtas_st(vcpu->kvm, rets, 0, 0); /* Success */
	rtas_st(vcpu->kvm, rets, 1, tm.tm_year + 1900);
	rtas_st(vcpu->kvm, rets, 2, tm.tm_mon + 1);
	rtas_st(vcpu->kvm, rets, 3, tm.tm_mday);
	rtas_st(vcpu->kvm, rets, 4, tm.tm_hour);
	rtas_st(vcpu->kvm, rets, 5, tm.tm_min);
	rtas_st(vcpu->kvm, rets, 6, tm.tm_sec);
	rtas_st(vcpu->kvm, rets, 7, 0);
}

static void rtas_set_time_of_day(struct kvm_cpu *vcpu,
                                 uint32_t token, uint32_t nargs,
                                 target_ulong args,
                                 uint32_t nret, target_ulong rets)
{
	pr_warning("%s called; TOD set ignored.\n", __FUNCTION__);
}

static void rtas_power_off(struct kvm_cpu *vcpu,
                           uint32_t token, uint32_t nargs, target_ulong args,
                           uint32_t nret, target_ulong rets)
{
	if (nargs != 2 || nret != 1) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}
	kvm_cpu__reboot(vcpu->kvm);
}

static void rtas_query_cpu_stopped_state(struct kvm_cpu *vcpu,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args,
                                         uint32_t nret, target_ulong rets)
{
	if (nargs != 1 || nret != 2) {
		rtas_st(vcpu->kvm, rets, 0, -3);
		return;
	}

	/*
	 * Can read id = rtas_ld(vcpu->kvm, args, 0), but
	 * we currently start all CPUs.  So just return true.
	 */
	rtas_st(vcpu->kvm, rets, 0, 0);
	rtas_st(vcpu->kvm, rets, 1, 2);
}

static void rtas_start_cpu(struct kvm_cpu *vcpu,
                           uint32_t token, uint32_t nargs,
                           target_ulong args,
                           uint32_t nret, target_ulong rets)
{
	die(__FUNCTION__);
}

target_ulong spapr_rtas_call(struct kvm_cpu *vcpu,
                             uint32_t token, uint32_t nargs, target_ulong args,
                             uint32_t nret, target_ulong rets)
{
	if ((token >= TOKEN_BASE)
	    && ((token - TOKEN_BASE) < TOKEN_MAX)) {
		struct rtas_call *call = rtas_table + (token - TOKEN_BASE);

		if (call->fn) {
			call->fn(vcpu, token, nargs, args, nret, rets);
			return H_SUCCESS;
		}
	}

	/*
	 * HACK: Some Linux early debug code uses RTAS display-character,
	 * but assumes the token value is 0xa (which it is on some real
	 * machines) without looking it up in the device tree.  This
	 * special case makes this work
	 */
	if (token == 0xa) {
		rtas_display_character(vcpu, 0xa, nargs, args, nret, rets);
		return H_SUCCESS;
	}

	hcall_dprintf("Unknown RTAS token 0x%x\n", token);
	rtas_st(vcpu->kvm, rets, 0, -3);
	return H_PARAMETER;
}

void spapr_rtas_register(const char *name, spapr_rtas_fn fn)
{
	assert(rtas_next < (rtas_table + TOKEN_MAX));

	rtas_next->name = name;
	rtas_next->fn = fn;

	rtas_next++;
}

/*
 * This is called from the context of an open /rtas node, in order to add
 * properties for the rtas call tokens.
 */
int spapr_rtas_fdt_setup(struct kvm *kvm, void *fdt)
{
	int ret;
	int i;

	for (i = 0; i < TOKEN_MAX; i++) {
		struct rtas_call *call = &rtas_table[i];

		if (!call->fn) {
			continue;
		}

		ret = fdt_property_cell(fdt, call->name, i + TOKEN_BASE);

		if (ret < 0) {
			pr_warning("Couldn't add rtas token for %s: %s\n",
				   call->name, fdt_strerror(ret));
			return ret;
		}

	}
	return 0;
}

void register_core_rtas(void)
{
	spapr_rtas_register("display-character", rtas_display_character);
	spapr_rtas_register("get-time-of-day", rtas_get_time_of_day);
	spapr_rtas_register("set-time-of-day", rtas_set_time_of_day);
	spapr_rtas_register("power-off", rtas_power_off);
	spapr_rtas_register("query-cpu-stopped-state",
			    rtas_query_cpu_stopped_state);
	spapr_rtas_register("start-cpu", rtas_start_cpu);
#ifdef RTAS_CONSOLE
	/* These are unused: We do console I/O via hcalls, not rtas. */
	spapr_rtas_register("put-term-char", rtas_put_term_char);
	spapr_rtas_register("get-term-char", rtas_get_term_char);
#endif
}
