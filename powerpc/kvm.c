/*
 * PPC64 (SPAPR) platform support
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * Portions of FDT setup borrowed from QEMU, copyright 2010 David Gibson, IBM
 * Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/kvm.h"
#include "kvm/util.h"
#include "libfdt.h"
#include "cpu_info.h"

#include "spapr.h"
#include "spapr_hvcons.h"
#include "spapr_pci.h"

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

#define HPT_ORDER 24

#define HUGETLBFS_PATH "/var/lib/hugetlbfs/global/pagesize-16MB/"

#define PHANDLE_XICP		0x00001111

static char kern_cmdline[2048];

struct kvm_ext kvm_req_ext[] = {
	{ DEFINE_KVM_EXT(KVM_CAP_PPC_UNSET_IRQ) },
	{ DEFINE_KVM_EXT(KVM_CAP_PPC_IRQ_LEVEL) },
	{ 0, 0 }
};

static uint32_t mfpvr(void)
{
	uint32_t r;
	asm volatile ("mfpvr %0" : "=r"(r));
	return r;
}

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
	unsigned long hpt;

	kvm->ram_size		= ram_size;

	/* Map "default" hugetblfs path to the standard 16M mount point */
	if (hugetlbfs_path && !strcmp(hugetlbfs_path, "default"))
		hugetlbfs_path = HUGETLBFS_PATH;

	kvm->ram_start = mmap_anon_or_hugetlbfs(kvm, hugetlbfs_path, kvm->ram_size);

	if (kvm->ram_start == MAP_FAILED)
		die("Couldn't map %lld bytes for RAM (%d)\n",
		    kvm->ram_size, errno);

	/* FDT goes at top of memory, RTAS just below */
	kvm->arch.fdt_gra = kvm->ram_size - FDT_MAX_SIZE;
	/* FIXME: Not all PPC systems have RTAS */
	kvm->arch.rtas_gra = kvm->arch.fdt_gra - RTAS_MAX_SIZE;
	madvise(kvm->ram_start, kvm->ram_size, MADV_MERGEABLE);

	/* FIXME:  SPAPR-PR specific; allocate a guest HPT. */
	if (posix_memalign((void **)&hpt, (1<<HPT_ORDER), (1<<HPT_ORDER)))
		die("Can't allocate %d bytes for HPT\n", (1<<HPT_ORDER));

	kvm->arch.sdr1 = ((hpt + 0x3ffffULL) & ~0x3ffffULL) | (HPT_ORDER-18);

	kvm->arch.pvr = mfpvr();

	/* FIXME: This is book3s-specific */
	cap_ppc_rma = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_PPC_RMA);
	if (cap_ppc_rma == 2)
		die("Need contiguous RMA allocation on this hardware, "
		    "which is not yet supported.");

	/* Do these before FDT setup, IRQ setup, etc. */
	/* FIXME: SPAPR-specific */
	hypercall_init();
	register_core_rtas();
	/* Now that hypercalls are initialised, register a couple for the console: */
	spapr_hvcons_init();
	spapr_create_phb(kvm, "pci", SPAPR_PCI_BUID,
			 SPAPR_PCI_MEM_WIN_ADDR,
			 SPAPR_PCI_MEM_WIN_SIZE,
			 SPAPR_PCI_IO_WIN_ADDR,
			 SPAPR_PCI_IO_WIN_SIZE);
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	munmap(kvm->ram_start, kvm->ram_size);
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	kvm__irq_line(kvm, irq, 1);
	kvm__irq_line(kvm, irq, 0);
}

void kvm__arch_periodic_poll(struct kvm *kvm)
{
	/* FIXME: Should register callbacks to platform-specific polls */
	spapr_hvcons_poll(kvm);
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
		kvm->arch.initrd_gra = INITRD_LOAD_ADDR;
		kvm->arch.initrd_size = p-i_start;
	} else {
		kvm->arch.initrd_size = 0;
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

struct fdt_prop {
	void *value;
	int size;
};

static void generate_segment_page_sizes(struct kvm_ppc_smmu_info *info, struct fdt_prop *prop)
{
	struct kvm_ppc_one_seg_page_size *sps;
	int i, j, size;
	u32 *p;

	for (size = 0, i = 0; i < KVM_PPC_PAGE_SIZES_MAX_SZ; i++) {
		sps = &info->sps[i];

		if (sps->page_shift == 0)
			break;

		/* page shift, slb enc & count */
		size += 3;

		for (j = 0; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++) {
			if (info->sps[i].enc[j].page_shift == 0)
				break;

			/* page shift & pte enc */
			size += 2;
		}
	}

	if (!size) {
		prop->value = NULL;
		prop->size = 0;
		return;
	}

	/* Convert size to bytes */
	prop->size = size * sizeof(u32);

	prop->value = malloc(prop->size);
	if (!prop->value)
		die_perror("malloc failed");

	p = (u32 *)prop->value;
	for (i = 0; i < KVM_PPC_PAGE_SIZES_MAX_SZ; i++) {
		sps = &info->sps[i];

		if (sps->page_shift == 0)
			break;

		*p++ = sps->page_shift;
		*p++ = sps->slb_enc;

		for (j = 0; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++)
			if (!info->sps[i].enc[j].page_shift)
				break;

		*p++ = j;	/* count of enc */

		for (j = 0; j < KVM_PPC_PAGE_SIZES_MAX_SZ; j++) {
			if (!info->sps[i].enc[j].page_shift)
				break;

			*p++ = info->sps[i].enc[j].page_shift;
			*p++ = info->sps[i].enc[j].pte_enc;
		}
	}
}

#define SMT_THREADS 4

/*
 * Set up the FDT for the kernel: This function is currently fairly SPAPR-heavy,
 * and whilst most PPC targets will require CPU/memory nodes, others like RTAS
 * should eventually be added separately.
 */
static int setup_fdt(struct kvm *kvm)
{
	uint64_t 	mem_reg_property[] = { 0, cpu_to_be64(kvm->ram_size) };
	int 		smp_cpus = kvm->nrcpus;
	uint32_t	int_server_ranges_prop[] = {0, cpu_to_be32(smp_cpus)};
	char 		hypertas_prop_kvm[] = "hcall-pft\0hcall-term\0"
		"hcall-dabr\0hcall-interrupt\0hcall-tce\0hcall-vio\0"
		"hcall-splpar\0hcall-bulk";
	int 		i, j;
	char 		cpu_name[30];
	u8		staging_fdt[FDT_MAX_SIZE];
	struct cpu_info *cpu_info = find_cpu_info(kvm);
	struct fdt_prop segment_page_sizes;
	u32 segment_sizes_1T[] = {0x1c, 0x28, 0xffffffff, 0xffffffff};

	/* Generate an appropriate DT at kvm->arch.fdt_gra */
	void *fdt_dest = guest_flat_to_host(kvm, kvm->arch.fdt_gra);
	void *fdt = staging_fdt;

	_FDT(fdt_create(fdt, FDT_MAX_SIZE));
	_FDT(fdt_finish_reservemap(fdt));

	_FDT(fdt_begin_node(fdt, ""));

	_FDT(fdt_property_string(fdt, "device_type", "chrp"));
	_FDT(fdt_property_string(fdt, "model", "IBM pSeries (kvmtool)"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));

	/* RTAS */
	_FDT(fdt_begin_node(fdt, "rtas"));
	/* This is what the kernel uses to switch 'We're an LPAR'! */
        _FDT(fdt_property(fdt, "ibm,hypertas-functions", hypertas_prop_kvm,
                           sizeof(hypertas_prop_kvm)));
	_FDT(fdt_property_cell(fdt, "linux,rtas-base", kvm->arch.rtas_gra));
	_FDT(fdt_property_cell(fdt, "linux,rtas-entry", kvm->arch.rtas_gra));
	_FDT(fdt_property_cell(fdt, "rtas-size", kvm->arch.rtas_size));
	/* Now add properties for all RTAS tokens: */
	if (spapr_rtas_fdt_setup(kvm, fdt))
		die("Couldn't create RTAS FDT properties\n");

	_FDT(fdt_end_node(fdt));

	/* /chosen */
	_FDT(fdt_begin_node(fdt, "chosen"));
	/* cmdline */
	_FDT(fdt_property_string(fdt, "bootargs", kern_cmdline));
	/* Initrd */
	if (kvm->arch.initrd_size != 0) {
		uint32_t ird_st_prop = cpu_to_be32(kvm->arch.initrd_gra);
		uint32_t ird_end_prop = cpu_to_be32(kvm->arch.initrd_gra +
						    kvm->arch.initrd_size);
		_FDT(fdt_property(fdt, "linux,initrd-start",
				   &ird_st_prop, sizeof(ird_st_prop)));
		_FDT(fdt_property(fdt, "linux,initrd-end",
				   &ird_end_prop, sizeof(ird_end_prop)));
	}

	/*
	 * stdout-path: This is assuming we're using the HV console.  Also, the
	 * address is hardwired until we do a VIO bus.
	 */
	_FDT(fdt_property_string(fdt, "linux,stdout-path",
				 "/vdevice/vty@30000000"));
	_FDT(fdt_end_node(fdt));

	/*
	 * Memory: We don't alloc. a separate RMA yet.  If we ever need to
	 * (CAP_PPC_RMA == 2) then have one memory node for 0->RMAsize, and
	 * another RMAsize->endOfMem.
	 */
	_FDT(fdt_begin_node(fdt, "memory@0"));
	_FDT(fdt_property_string(fdt, "device_type", "memory"));
	_FDT(fdt_property(fdt, "reg", mem_reg_property,
			  sizeof(mem_reg_property)));
	_FDT(fdt_end_node(fdt));

	generate_segment_page_sizes(&cpu_info->mmu_info, &segment_page_sizes);

	/* CPUs */
	_FDT(fdt_begin_node(fdt, "cpus"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x0));

	for (i = 0; i < smp_cpus; i += SMT_THREADS) {
		int32_t pft_size_prop[] = { 0, HPT_ORDER };
		uint32_t servers_prop[SMT_THREADS];
		uint32_t gservers_prop[SMT_THREADS * 2];
		int threads = (smp_cpus - i) >= SMT_THREADS ? SMT_THREADS :
			smp_cpus - i;

		sprintf(cpu_name, "PowerPC,%s@%d", cpu_info->name, i);
		_FDT(fdt_begin_node(fdt, cpu_name));
		sprintf(cpu_name, "PowerPC,%s", cpu_info->name);
		_FDT(fdt_property_string(fdt, "name", cpu_name));
		_FDT(fdt_property_string(fdt, "device_type", "cpu"));

		_FDT(fdt_property_cell(fdt, "reg", i));
		_FDT(fdt_property_cell(fdt, "cpu-version", kvm->arch.pvr));

		_FDT(fdt_property_cell(fdt, "dcache-block-size", cpu_info->d_bsize));
		_FDT(fdt_property_cell(fdt, "icache-block-size", cpu_info->i_bsize));

		_FDT(fdt_property_cell(fdt, "timebase-frequency", cpu_info->tb_freq));
		/* Lies, but safeish lies! */
		_FDT(fdt_property_cell(fdt, "clock-frequency", 0xddbab200));

		if (cpu_info->mmu_info.slb_size)
			_FDT(fdt_property_cell(fdt, "ibm,slb-size", cpu_info->mmu_info.slb_size));

		/*
		 * HPT size is hardwired; KVM currently fixes it at 16MB but the
		 * moment that changes we'll need to read it out of the kernel.
		 */
		_FDT(fdt_property(fdt, "ibm,pft-size", pft_size_prop,
				  sizeof(pft_size_prop)));

		_FDT(fdt_property_string(fdt, "status", "okay"));
		_FDT(fdt_property(fdt, "64-bit", NULL, 0));
		/* A server for each thread in this core */
		for (j = 0; j < SMT_THREADS; j++) {
			servers_prop[j] = cpu_to_be32(i+j);
			/*
			 * Hack borrowed from QEMU, direct the group queues back
			 * to cpu 0:
			 */
			gservers_prop[j*2] = cpu_to_be32(i+j);
			gservers_prop[j*2 + 1] = 0;
		}
		_FDT(fdt_property(fdt, "ibm,ppc-interrupt-server#s",
				   servers_prop, threads * sizeof(uint32_t)));
		_FDT(fdt_property(fdt, "ibm,ppc-interrupt-gserver#s",
				  gservers_prop,
				  threads * 2 * sizeof(uint32_t)));

		if (segment_page_sizes.value)
			_FDT(fdt_property(fdt, "ibm,segment-page-sizes",
					  segment_page_sizes.value,
					  segment_page_sizes.size));

		if (cpu_info->mmu_info.flags & KVM_PPC_1T_SEGMENTS)
			_FDT(fdt_property(fdt, "ibm,processor-segment-sizes",
					  segment_sizes_1T, sizeof(segment_sizes_1T)));

		/* VSX / DFP options: */
		if (cpu_info->flags & CPUINFO_FLAG_VMX)
			_FDT(fdt_property_cell(fdt, "ibm,vmx",
					       (cpu_info->flags &
						CPUINFO_FLAG_VSX) ? 2 : 1));
		if (cpu_info->flags & CPUINFO_FLAG_DFP)
			_FDT(fdt_property_cell(fdt, "ibm,dfp", 0x1));
		_FDT(fdt_end_node(fdt));
	}
	_FDT(fdt_end_node(fdt));

	/* IRQ controller */
	_FDT(fdt_begin_node(fdt, "interrupt-controller@0"));

	_FDT(fdt_property_string(fdt, "device_type",
				 "PowerPC-External-Interrupt-Presentation"));
	_FDT(fdt_property_string(fdt, "compatible", "IBM,ppc-xicp"));
	_FDT(fdt_property_cell(fdt, "reg", 0));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property(fdt, "ibm,interrupt-server-ranges",
			   int_server_ranges_prop,
			   sizeof(int_server_ranges_prop)));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", 2));
	_FDT(fdt_property_cell(fdt, "linux,phandle", PHANDLE_XICP));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_XICP));
	_FDT(fdt_end_node(fdt));

	/*
	 * VIO: See comment in linux,stdout-path; we don't yet represent a VIO
	 * bus/address allocation so addresses are hardwired here.
	 */
	_FDT(fdt_begin_node(fdt, "vdevice"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x0));
	_FDT(fdt_property_string(fdt, "device_type", "vdevice"));
	_FDT(fdt_property_string(fdt, "compatible", "IBM,vdevice"));
	_FDT(fdt_begin_node(fdt, "vty@30000000"));
	_FDT(fdt_property_string(fdt, "name", "vty"));
	_FDT(fdt_property_string(fdt, "device_type", "serial"));
	_FDT(fdt_property_string(fdt, "compatible", "hvterm1"));
	_FDT(fdt_property_cell(fdt, "reg", 0x30000000));
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_end_node(fdt));

	/* Finalise: */
	_FDT(fdt_end_node(fdt)); /* Root node */
	_FDT(fdt_finish(fdt));

	_FDT(fdt_open_into(fdt, fdt_dest, FDT_MAX_SIZE));

	/* PCI */
	if (spapr_populate_pci_devices(kvm, PHANDLE_XICP, fdt_dest))
		die("Fail populating PCI device nodes");

	_FDT(fdt_add_mem_rsv(fdt_dest, kvm->arch.rtas_gra, kvm->arch.rtas_size));
	_FDT(fdt_pack(fdt_dest));

	free(segment_page_sizes.value);

	return 0;
}
firmware_init(setup_fdt);

/**
 * kvm__arch_setup_firmware
 */
int kvm__arch_setup_firmware(struct kvm *kvm)
{
	/*
	 * Set up RTAS stub.  All it is is a single hypercall:
	 *  0:   7c 64 1b 78     mr      r4,r3
	 *  4:   3c 60 00 00     lis     r3,0
	 *  8:   60 63 f0 00     ori     r3,r3,61440
	 *  c:   44 00 00 22     sc      1
	 * 10:   4e 80 00 20     blr
	 */
	uint32_t *rtas = guest_flat_to_host(kvm, kvm->arch.rtas_gra);

	rtas[0] = 0x7c641b78;
	rtas[1] = 0x3c600000;
	rtas[2] = 0x6063f000;
	rtas[3] = 0x44000022;
	rtas[4] = 0x4e800020;
	kvm->arch.rtas_size = 20;

	pr_info("Set up %ld bytes of RTAS at 0x%lx\n",
		kvm->arch.rtas_size, kvm->arch.rtas_gra);

	/* Load SLOF */

	return 0;
}

int kvm__arch_free_firmware(struct kvm *kvm)
{
	return 0;
}
