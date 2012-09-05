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

#include <kvm/kvm.h>
#include <sys/ioctl.h>

#include "cpu_info.h"
#include "kvm/util.h"

/* POWER7 */

static struct cpu_info cpu_power7_info = {
	.name = "POWER7",
	.tb_freq = 512000000,
	.d_bsize = 128,
	.i_bsize = 128,
	.flags = CPUINFO_FLAG_DFP | CPUINFO_FLAG_VSX | CPUINFO_FLAG_VMX,
	.mmu_info = {
		.flags = KVM_PPC_PAGE_SIZES_REAL | KVM_PPC_1T_SEGMENTS,
		.slb_size = 32,
	},
};

/* PPC970/G5 */

static struct cpu_info cpu_970_info = {
	.name = "G5",
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

/* If we can't query the kernel for supported page sizes assume 4K and 16M */
static struct kvm_ppc_one_seg_page_size fallback_sps[] = {
	[0] = {
		.page_shift = 12,
		.slb_enc    = 0,
		.enc =  {
			[0] = {
				.page_shift = 12,
				.pte_enc    = 0,
			},
		},
	},
	[1] = {
		.page_shift = 24,
		.slb_enc    = 0x100,
		.enc =  {
			[0] = {
				.page_shift = 24,
				.pte_enc    = 0,
			},
		},
	},
};


static void setup_mmu_info(struct kvm *kvm, struct cpu_info *cpu_info)
{
	static struct kvm_ppc_smmu_info *mmu_info;
	struct kvm_ppc_one_seg_page_size *sps;
	int i, j, k, valid;

	if (!kvm__supports_extension(kvm, KVM_CAP_PPC_GET_SMMU_INFO)) {
		memcpy(&cpu_info->mmu_info.sps, fallback_sps, sizeof(fallback_sps));
	} else if (ioctl(kvm->vm_fd, KVM_PPC_GET_SMMU_INFO, &cpu_info->mmu_info) < 0) {
			die_perror("KVM_PPC_GET_SMMU_INFO failed");
	}

	mmu_info = &cpu_info->mmu_info;

	if (!(mmu_info->flags & KVM_PPC_PAGE_SIZES_REAL))
		/* Guest pages are not restricted by the backing page size */
		return;

	/* Filter based on backing page size */

	for (i = 0; i < KVM_PPC_PAGE_SIZES_MAX_SZ; i++) {
		sps = &mmu_info->sps[i];

		if (!sps->page_shift)
			break;

		if (kvm->ram_pagesize < (1ul << sps->page_shift)) {
			/* Mark the whole segment size invalid */
			sps->page_shift = 0;
			continue;
		}

		/* Check each page size for the segment */
		for (j = 0, valid = 0; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++) {
			if (!sps->enc[j].page_shift)
				break;

			if (kvm->ram_pagesize < (1ul << sps->enc[j].page_shift))
				sps->enc[j].page_shift = 0;
			else
				valid++;
		}

		if (!valid) {
			/* Mark the whole segment size invalid */
			sps->page_shift = 0;
			continue;
		}

		/* Mark any trailing entries invalid if we broke out early */
		for (k = j; k < KVM_PPC_PAGE_SIZES_MAX_SZ; k++)
			sps->enc[k].page_shift = 0;

		/* Collapse holes */
		for (j = 0; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++) {
			if (sps->enc[j].page_shift)
				continue;

			for (k = j + 1; k < KVM_PPC_PAGE_SIZES_MAX_SZ; k++) {
				if (sps->enc[k].page_shift) {
					sps->enc[j] = sps->enc[k];
					sps->enc[k].page_shift = 0;
					break;
				}
			}
		}
	}

	/* Mark any trailing entries invalid if we broke out early */
	for (j = i; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++)
		mmu_info->sps[j].page_shift = 0;

	/* Collapse holes */
	for (i = 0; i < KVM_PPC_PAGE_SIZES_MAX_SZ; i++) {
		if (mmu_info->sps[i].page_shift)
			continue;

		for (j = i + 1; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++) {
			if (mmu_info->sps[j].page_shift) {
				mmu_info->sps[i] = mmu_info->sps[j];
				mmu_info->sps[j].page_shift = 0;
				break;
			}
		}
	}
}

struct cpu_info *find_cpu_info(struct kvm *kvm)
{
	struct cpu_info *info;
	unsigned int i;
	u32 pvr = kvm->arch.pvr;

	for (info = NULL, i = 0; i < ARRAY_SIZE(host_pvr_info); i++) {
		if ((pvr & host_pvr_info[i].pvr_mask) == host_pvr_info[i].pvr) {
			info = host_pvr_info[i].cpu_info;
			break;
		}
	}

	/* Didn't find anything? Rut-ro. */
	if (!info) {
		pr_warning("Host CPU unsupported by kvmtool\n");
		info = &cpu_dummy_info;
	}

	setup_mmu_info(kvm, info);

	return info;
}
