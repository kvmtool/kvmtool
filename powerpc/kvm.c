/*
 * PPC64 (SPAPR) platform support
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/kvm.h"
#include "kvm/util.h"

#include <linux/kvm.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include <errno.h>

#include <linux/byteorder.h>
#include <libfdt.h>

#define HUGETLBFS_PATH "/var/lib/hugetlbfs/global/pagesize-16MB/"

static char kern_cmdline[2048];

struct kvm_ext kvm_req_ext[] = {
	{ 0, 0 }
};

bool kvm__arch_cpu_supports_vm(void)
{
	return true;
}

void kvm__init_ram(struct kvm *kvm)
{
	u64	phys_start, phys_size;
	void	*host_mem;

	phys_start = 0;
	phys_size  = kvm->ram_size;
	host_mem   = kvm->ram_start;

	/*
	 * We put MMIO at PPC_MMIO_START, high up.  Make sure that this doesn't
	 * crash into the end of RAM -- on PPC64 at least, this is so high
	 * (63TB!) that this is unlikely.
	 */
	if (phys_size >= PPC_MMIO_START)
		die("Too much memory (%lld, what a nice problem): "
		    "overlaps MMIO!\n",
		    phys_size);

	kvm__register_mem(kvm, phys_start, phys_size, host_mem);
}

void kvm__arch_set_cmdline(char *cmdline, bool video)
{
	/* We don't need anything unusual in here. */
}

/* Architecture-specific KVM init */
void kvm__arch_init(struct kvm *kvm, const char *hugetlbfs_path, u64 ram_size)
{
	int cap_ppc_rma;

	kvm->ram_size		= ram_size;

	/*
	 * Currently, we must map from hugetlbfs; if --hugetlbfs not specified,
	 * try a default path:
	 */
	if (!hugetlbfs_path) {
		hugetlbfs_path = HUGETLBFS_PATH;
		pr_info("Using default %s for memory", hugetlbfs_path);
	}

	kvm->ram_start = mmap_hugetlbfs(hugetlbfs_path, kvm->ram_size);
	if (kvm->ram_start == MAP_FAILED)
		die("Couldn't map %lld bytes for RAM (%d)\n",
		    kvm->ram_size, errno);

	/* FDT goes at top of memory, RTAS just below */
	kvm->fdt_gra = kvm->ram_size - FDT_MAX_SIZE;
	/* FIXME: Not all PPC systems have RTAS */
	kvm->rtas_gra = kvm->fdt_gra - RTAS_MAX_SIZE;
	madvise(kvm->ram_start, kvm->ram_size, MADV_MERGEABLE);

	/* FIXME: This is book3s-specific */
	cap_ppc_rma = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_PPC_RMA);
	if (cap_ppc_rma == 2)
		die("Need contiguous RMA allocation on this hardware, "
		    "which is not yet supported.");
}

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	fprintf(stderr, "irq_line(%d, %d)\n", irq, level);
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	kvm__irq_line(kvm, irq, 1);
	kvm__irq_line(kvm, irq, 0);
}

int load_flat_binary(struct kvm *kvm, int fd_kernel, int fd_initrd, const char *kernel_cmdline)
{
	void *p;
	void *k_start;
	void *i_start;
	int nr;

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	p = k_start = guest_flat_to_host(kvm, KERNEL_LOAD_ADDR);

	while ((nr = read(fd_kernel, p, 65536)) > 0)
		p += nr;

	pr_info("Loaded kernel to 0x%x (%ld bytes)", KERNEL_LOAD_ADDR, p-k_start);

	if (fd_initrd != -1) {
		if (lseek(fd_initrd, 0, SEEK_SET) < 0)
			die_perror("lseek");

		if (p-k_start > INITRD_LOAD_ADDR)
			die("Kernel overlaps initrd!");

		/* Round up kernel size to 8byte alignment, and load initrd right after. */
		i_start = p = guest_flat_to_host(kvm, INITRD_LOAD_ADDR);

		while (((nr = read(fd_initrd, p, 65536)) > 0) &&
		       p < (kvm->ram_start + kvm->ram_size))
			p += nr;

		if (p >= (kvm->ram_start + kvm->ram_size))
			die("initrd too big to contain in guest RAM.\n");

		pr_info("Loaded initrd to 0x%x (%ld bytes)",
			INITRD_LOAD_ADDR, p-i_start);
		kvm->initrd_gra = INITRD_LOAD_ADDR;
		kvm->initrd_size = p-i_start;
	} else {
		kvm->initrd_size = 0;
	}
	strncpy(kern_cmdline, kernel_cmdline, 2048);
	kern_cmdline[2047] = '\0';

	return true;
}

bool load_bzimage(struct kvm *kvm, int fd_kernel,
		  int fd_initrd, const char *kernel_cmdline, u16 vidmode)
{
	/* We don't support bzImages. */
	return false;
}

static void setup_fdt(struct kvm *kvm)
{

}

/**
 * kvm__arch_setup_firmware
 */
int kvm__arch_setup_firmware(struct kvm *kvm)
{
	/* Load RTAS */

	/* Load SLOF */

	/* Init FDT */
	setup_fdt(kvm);

	return 0;
}
