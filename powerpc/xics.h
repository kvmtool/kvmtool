/*
 * PAPR Virtualized Interrupt System, aka ICS/ICP aka xics
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef XICS_H
#define XICS_H

#define XICS_IPI        0x2

int xics_alloc_irqnum(void);

#endif
