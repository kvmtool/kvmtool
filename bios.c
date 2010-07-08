#include "kvm/kvm.h"
#include "kvm/interrupt.h"
#include "kvm/util.h"

#include <string.h>

static void bios_setup_irq_handler(struct kvm *kvm, unsigned int address,
				unsigned int irq, void *handler, unsigned int size)
{
	struct real_intr_desc intr_desc;
	void *p;

	p = guest_flat_to_host(kvm, address);
	memcpy(p, handler, size);
	intr_desc = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(address),
		.offset		= REAL_OFFSET(address),
	};
	interrupt_table__set(&kvm->interrupt_table, &intr_desc, irq);
}

void setup_bios(struct kvm *kvm)
{
	unsigned long address = MB_BIOS_BEGIN;
	struct real_intr_desc intr_desc;
	void *p;

	/*
	 * Setup a *fake* real mode vector table, it has only
	 * one real hadler which does just iret
	 */
	address = BIOS_NEXT_IRQ_ADDR(address, 0);
	p = guest_flat_to_host(kvm, address);
	memcpy(p, bios_intfake, bios_intfake_size);
	intr_desc = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(address),
		.offset		= REAL_OFFSET(address),
	};
	interrupt_table__setup(&kvm->interrupt_table, &intr_desc);

	/*
	 * int 0x10
	 */
	address = BIOS_NEXT_IRQ_ADDR(address, bios_intfake_size);
	bios_setup_irq_handler(kvm, address, 0x10, bios_int10, bios_int10_size);

	/*
	 * We don't have valid BIOS yet so we put one single memory
	 * region in e820 memory map
	 *
	 * int 0x15
	 */
	address = BIOS_NEXT_IRQ_ADDR(address, bios_int10_size);
	bios_setup_irq_handler(kvm, address, 0x15, bios_int15, bios_int15_size);

	p = guest_flat_to_host(kvm, 0);
	interrupt_table__copy(&kvm->interrupt_table, p, REAL_INTR_SIZE);
}
