/*
 * BIOS fake interrupt stub, it does nothing
 * and lockless
 */

#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
	.code16gcc

ENTRY(intfake)
	IRET
ENTRY_END(intfake)

