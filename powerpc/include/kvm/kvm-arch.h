/*
 * PPC64 architecture-specific definitions
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef KVM__KVM_ARCH_H
#define KVM__KVM_ARCH_H

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>

/*
 * MMIO lives after RAM, but it'd be nice if it didn't constantly move.
 * Choose a suitably high address, e.g. 63T...  This limits RAM size.
 */
#define PPC_MMIO_START			0x3F0000000000UL
#define PPC_MMIO_SIZE			0x010000000000UL

#define KERNEL_LOAD_ADDR        	0x0000000000000000
#define KERNEL_START_ADDR       	0x0000000000000000
#define KERNEL_SECONDARY_START_ADDR     0x0000000000000060
#define INITRD_LOAD_ADDR        	0x0000000002800000

#define FDT_MAX_SIZE            	0x10000
#define RTAS_MAX_SIZE           	0x10000

#define TIMEBASE_FREQ           	512000000ULL

#define KVM_MMIO_START			PPC_MMIO_START

/*
 * This is the address that pci_get_io_space_block() starts allocating
 * from.  Note that this is a PCI bus address.
 */
#define KVM_PCI_MMIO_AREA		0x1000000

struct spapr_phb;

struct kvm {
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	timer_t			timerid;	/* Posix timer for interrupts */

	int			nrcpus;		/* Number of cpus to run */

	u32			mem_slots;	/* for KVM_SET_USER_MEMORY_REGION */

	u64			ram_size;
	void			*ram_start;

	u64			sdr1;
	u32			pvr;

	bool			nmi_disabled;

	bool			single_step;

	const char		*vmlinux;
	struct disk_image       **disks;
	int                     nr_disks;
	unsigned long		rtas_gra;
	unsigned long		rtas_size;
	unsigned long		fdt_gra;
	unsigned long		initrd_gra;
	unsigned long		initrd_size;
	char			*name;
	int			vm_state;
	struct icp_state	*icp;
	struct spapr_phb	*phb;
};

/* Helper for the various bits of code that generate FDT nodes */
#define _FDT(exp)							\
	do {								\
		int ret = (exp);					\
		if (ret < 0) {						\
			die("Error creating device tree: %s: %s\n",	\
			    #exp, fdt_strerror(ret));			\
		}							\
	} while (0)

#endif /* KVM__KVM_ARCH_H */
