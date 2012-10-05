/*
 * PPC64 IRQ routines
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

#include <stddef.h>
#include <stdlib.h>

#include "kvm/pci.h"

#include "xics.h"
#include "spapr_pci.h"

/*
 * FIXME: The code in this file assumes an SPAPR guest, using XICS.  Make
 * generic & cope with multiple PPC platform types.
 */

static int pci_devs = 0;

int irq__register_device(u32 dev, u8 *num, u8 *pin, u8 *line)
{
	if (pci_devs >= PCI_MAX_DEVICES)
		die("Hit PCI device limit!\n");

	*num = pci_devs++;

	*pin = 1;
	/*
	 * Have I said how nasty I find this?  Line should be dontcare... PHB
	 * should determine which CPU/XICS IRQ to fire.
	 */
	*line = xics_alloc_irqnum();
	return 0;
}

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	die(__FUNCTION__);
	return 0;
}
