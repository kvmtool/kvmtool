/*
 * PPC64 ioport platform setup.  There isn't any! :-)
 *
 * Copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "kvm/ioport.h"

#include <stdlib.h>

void ioport__setup_arch(struct kvm *kvm)
{
	/* PPC has no legacy ioports to set up */
}
