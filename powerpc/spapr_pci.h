/*
 * SPAPR PHB definitions
 *
 * Modifications by Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef SPAPR_PCI_H
#define SPAPR_PCI_H

#include "kvm/kvm.h"
#include "spapr.h"
#include <inttypes.h>

/* With XICS, we can easily accomodate 1 IRQ per PCI device. */

#define SPAPR_PCI_NUM_LSI 256

struct spapr_phb {
	uint64_t buid;
	uint64_t mem_addr;
	uint64_t mem_size;
	uint64_t io_addr;
	uint64_t io_size;
};

void spapr_create_phb(struct kvm *kvm,
                      const char *busname, uint64_t buid,
                      uint64_t mem_win_addr, uint64_t mem_win_size,
                      uint64_t io_win_addr, uint64_t io_win_size);

int spapr_populate_pci_devices(struct kvm *kvm,
                               uint32_t xics_phandle,
                               void *fdt);

static inline bool spapr_phb_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write)
{
	if ((phys_addr >= SPAPR_PCI_IO_WIN_ADDR) &&
	    (phys_addr < SPAPR_PCI_IO_WIN_ADDR +
	     SPAPR_PCI_IO_WIN_SIZE)) {
		return kvm__emulate_io(kvm, phys_addr - SPAPR_PCI_IO_WIN_ADDR,
				       data, is_write ? KVM_EXIT_IO_OUT :
				       KVM_EXIT_IO_IN,
				       len, 1);
	} else if ((phys_addr >= SPAPR_PCI_MEM_WIN_ADDR) &&
		   (phys_addr < SPAPR_PCI_MEM_WIN_ADDR +
		    SPAPR_PCI_MEM_WIN_SIZE)) {
		return kvm__emulate_mmio(kvm, phys_addr - SPAPR_PCI_MEM_WIN_ADDR,
					 data, len, is_write);
	}
	return false;
}

#endif
