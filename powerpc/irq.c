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

int irq__register_device(u32 dev, u8 *num, u8 *pin, u8 *line)
{
	fprintf(stderr, "irq__register_device(%d, [%d], [%d], [%d]\n",
		dev, *num, *pin, *line);
	return 0;
}

void irq__init(struct kvm *kvm)
{
	fprintf(stderr, __func__);
}

int irq__add_msix_route(struct kvm *kvm, struct msi_msg *msg)
{
	die(__FUNCTION__);
	return 0;
}
