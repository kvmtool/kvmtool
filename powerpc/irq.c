/*
 * PPC64 IRQ routines
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/devices.h"
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

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	die(__FUNCTION__);
	return 0;
}
