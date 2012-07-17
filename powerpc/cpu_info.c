/*
 * PPC CPU identification
 *
 * This is a very simple "host CPU info" struct to get us going.
 * For the little host information we need, I don't want to grub about
 * parsing stuff in /proc/device-tree so just match host PVR to differentiate
 * PPC970 and POWER7 (which is all that's currently supported).
 *
 * Qemu does something similar but this is MUCH simpler!
 *
 * Copyright 2012 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "cpu_info.h"
#include "kvm/util.h"

/* POWER7 */

/*
 * Basic set of pages for POWER7.  It actually supports more but there were some
 * limitations as to which may be advertised to the guest.  FIXME when this
 * settles down -- for now use basic set:
 */
static u32 power7_page_sizes_prop[] = {0xc, 0x0, 0x1, 0xc, 0x0, 0x18, 0x100, 0x1, 0x18, 0x0};
/* POWER7 has 1T segments, so advertise these */
static u32 power7_segment_sizes_prop[] = {0x1c, 0x28, 0xffffffff, 0xffffffff};

static struct cpu_info cpu_power7_info = {
	.name = "POWER7",
	.page_sizes_prop = power7_page_sizes_prop,
	.page_sizes_prop_len = sizeof(power7_segment_sizes_prop),
	.segment_sizes_prop = power7_segment_sizes_prop,
	.segment_sizes_prop_len = sizeof(power7_segment_sizes_prop),
	.slb_size = 32,
	.tb_freq = 512000000,
	.d_bsize = 128,
	.i_bsize = 128,
	.flags = CPUINFO_FLAG_DFP | CPUINFO_FLAG_VSX | CPUINFO_FLAG_VMX,
};

/* PPC970/G5 */

static u32 g5_page_sizes_prop[] = {0xc, 0x0, 0x1, 0xc, 0x0, 0x18, 0x100, 0x1, 0x18, 0x0};

static struct cpu_info cpu_970_info = {
	.name = "G5",
	.page_sizes_prop = g5_page_sizes_prop,
	.page_sizes_prop_len = sizeof(g5_page_sizes_prop),
	.segment_sizes_prop = NULL /* no segment sizes prop, use defaults */,
	.segment_sizes_prop_len = 0,
	.slb_size = 0,
	.tb_freq = 33333333,
	.d_bsize = 128,
	.i_bsize = 128,
	.flags = CPUINFO_FLAG_VMX,
};

/* This is a default catchall for 'no match' on PVR: */
static struct cpu_info cpu_dummy_info = { .name = "unknown" };

static struct pvr_info host_pvr_info[] = {
	{ 0xffffffff, 0x0f000003, &cpu_power7_info },
	{ 0xffff0000, 0x003f0000, &cpu_power7_info },
	{ 0xffff0000, 0x004a0000, &cpu_power7_info },
	{ 0xffff0000, 0x00390000, &cpu_970_info },
	{ 0xffff0000, 0x003c0000, &cpu_970_info },
        { 0xffff0000, 0x00440000, &cpu_970_info },
        { 0xffff0000, 0x00450000, &cpu_970_info },
};

struct cpu_info *find_cpu_info(u32 pvr)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(host_pvr_info); i++) {
		if ((pvr & host_pvr_info[i].pvr_mask) == host_pvr_info[i].pvr) {
			return host_pvr_info[i].cpu_info;
		}
	}

	/* Didn't find anything? Rut-ro. */
	pr_warning("Host CPU unsupported by kvmtool\n");

	return &cpu_dummy_info;
}
