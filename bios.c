#include "kvm/kvm.h"
#include "kvm/interrupt.h"
#include "kvm/util.h"

#include <string.h>

#include "bios/bios-rom.h"

struct irq_handler {
	unsigned long		address;
	unsigned int		irq;
	void			*handler;
	size_t			size;
};

#define BIOS_IRQ_ADDR(name) (MB_BIOS_BEGIN + BIOS_OFFSET__##name)
#define BIOS_IRQ_FUNC(name) ((char *)&bios_rom[BIOS_OFFSET__##name])
#define BIOS_IRQ_SIZE(name) (BIOS_ENTRY_SIZE(BIOS_OFFSET__##name))

#define DEFINE_BIOS_IRQ_HANDLER(_irq, _handler)			\
	{							\
		.irq		= _irq,				\
		.address	= BIOS_IRQ_ADDR(_handler),	\
		.handler	= BIOS_IRQ_FUNC(_handler),	\
		.size		= BIOS_IRQ_SIZE(_handler),	\
	}

static struct irq_handler bios_irq_handlers[] = {
	DEFINE_BIOS_IRQ_HANDLER(0x10, bios_int10),
	DEFINE_BIOS_IRQ_HANDLER(0x15, bios_int15),
};

static void setup_irq_handler(struct kvm *kvm, struct irq_handler *handler)
{
	struct real_intr_desc intr_desc;
	void *p;

	p	= guest_flat_to_host(kvm, handler->address);
	memcpy(p, handler->handler, handler->size);

	intr_desc = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(handler->address),
		.offset		= REAL_OFFSET(handler->address),
	};

	interrupt_table__set(&kvm->interrupt_table, &intr_desc, handler->irq);
}

void setup_bios(struct kvm *kvm)
{
	unsigned long address = MB_BIOS_BEGIN;
	struct real_intr_desc intr_desc;
	unsigned int i;
	void *p;

	/*
	 * before anything else -- clean some known areas
	 * we definitely don't want any trash here
	 */
	p = guest_flat_to_host(kvm, BDA_START);
	memset(p, 0, BDA_END - BDA_START);

	p = guest_flat_to_host(kvm, EBDA_START);
	memset(p, 0, EBDA_END - EBDA_START);

	p = guest_flat_to_host(kvm, MB_BIOS_BEGIN);
	memset(p, 0, MB_BIOS_END - MB_BIOS_BEGIN);

	p = guest_flat_to_host(kvm, VGA_ROM_BEGIN);
	memset(p, 0, VGA_ROM_END - VGA_ROM_BEGIN);

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

	for (i = 0; i < ARRAY_SIZE(bios_irq_handlers); i++)
		setup_irq_handler(kvm, &bios_irq_handlers[i]);

	/* we almost done */
	p = guest_flat_to_host(kvm, 0);
	interrupt_table__copy(&kvm->interrupt_table, p, REAL_INTR_SIZE);
}
