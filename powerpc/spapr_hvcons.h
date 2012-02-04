/*
 * SPAPR HV console
 *
 * Copyright (c) 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef spapr_hvcons_H
#define spapr_hvcons_H

#include "kvm/kvm.h"

void spapr_hvcons_init(void);
void spapr_hvcons_poll(struct kvm *kvm);

#endif
