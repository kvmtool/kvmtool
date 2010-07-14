#include "kvm/kvm.h"
#include "kvm/interrupt.h"
#include "kvm/util.h"

#include <string.h>

#include "bios/bios-rom.h"

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

#define BIOS_IRQ_ADDR(name) (MB_BIOS_BEGIN + BIOS_OFFSET__##name)
#define BIOS_IRQ_FUNC(name) ((char *)&bios_rom[BIOS_OFFSET__##name])
#define BIOS_IRQ_SIZE(name) (BIOS_ENTRY_SIZE(BIOS_OFFSET__##name))

void setup_bios(struct kvm *kvm)
{
	unsigned long address = MB_BIOS_BEGIN;
	struct real_intr_desc intr_desc;
	void *p;

	/* just copy the bios rom into the place */
	p = guest_flat_to_host(kvm, MB_BIOS_BEGIN);
	memcpy(p, bios_rom, bios_rom_size);

	/*
	 * Setup a *fake* real mode vector table, it has only
	 * one real hadler which does just iret
	 */
	address = BIOS_IRQ_ADDR(bios_intfake);
	intr_desc = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(address),
		.offset		= REAL_OFFSET(address),
	};
	interrupt_table__setup(&kvm->interrupt_table, &intr_desc);

	/* int 0x10 */
	address = BIOS_IRQ_ADDR(bios_int10);
	bios_setup_irq_handler(kvm, address, 0x10, BIOS_IRQ_FUNC(bios_int10), BIOS_IRQ_SIZE(bios_int10));

	/*
	 * e820 memory map
	 *
	 * int 0x15
	 */
	address = BIOS_IRQ_ADDR(bios_int15);
	bios_setup_irq_handler(kvm, address, 0x15, BIOS_IRQ_FUNC(bios_int15), BIOS_IRQ_SIZE(bios_int15));

	/* we almost done */
	p = guest_flat_to_host(kvm, 0);
	interrupt_table__copy(&kvm->interrupt_table, p, REAL_INTR_SIZE);
}
