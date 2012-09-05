#include "kvm/kvm.h"
#include "kvm/boot-protocol.h"
#include "kvm/e820.h"
#include "kvm/interrupt.h"
#include "kvm/util.h"

#include <string.h>
#include <asm/e820.h>

#include "bios/bios-rom.h"

struct irq_handler {
	unsigned long		address;
	unsigned int		irq;
	void			*handler;
	size_t			size;
};

#define BIOS_IRQ_PA_ADDR(name)	(MB_BIOS_BEGIN + BIOS_OFFSET__##name)
#define BIOS_IRQ_FUNC(name)	((char *)&bios_rom[BIOS_OFFSET__##name])
#define BIOS_IRQ_SIZE(name)	(BIOS_ENTRY_SIZE(BIOS_OFFSET__##name))

#define DEFINE_BIOS_IRQ_HANDLER(_irq, _handler)			\
	{							\
		.irq		= _irq,				\
		.address	= BIOS_IRQ_PA_ADDR(_handler),	\
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

	p = guest_flat_to_host(kvm, handler->address);
	memcpy(p, handler->handler, handler->size);

	intr_desc = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(MB_BIOS_BEGIN),
		.offset		= handler->address - MB_BIOS_BEGIN,
	};

	DIE_IF((handler->address - MB_BIOS_BEGIN) > 0xffffUL);

	interrupt_table__set(&kvm->arch.interrupt_table, &intr_desc, handler->irq);
}

/**
 * e820_setup - setup some simple E820 memory map
 * @kvm - guest system descriptor
 */
static void e820_setup(struct kvm *kvm)
{
	struct e820map *e820;
	struct e820entry *mem_map;
	unsigned int i = 0;

	e820		= guest_flat_to_host(kvm, E820_MAP_START);
	mem_map		= e820->map;

	mem_map[i++]	= (struct e820entry) {
		.addr		= REAL_MODE_IVT_BEGIN,
		.size		= EBDA_START - REAL_MODE_IVT_BEGIN,
		.type		= E820_RAM,
	};
	mem_map[i++]	= (struct e820entry) {
		.addr		= EBDA_START,
		.size		= VGA_RAM_BEGIN - EBDA_START,
		.type		= E820_RESERVED,
	};
	mem_map[i++]	= (struct e820entry) {
		.addr		= MB_BIOS_BEGIN,
		.size		= MB_BIOS_END - MB_BIOS_BEGIN,
		.type		= E820_RESERVED,
	};
	if (kvm->ram_size < KVM_32BIT_GAP_START) {
		mem_map[i++]	= (struct e820entry) {
			.addr		= BZ_KERNEL_START,
			.size		= kvm->ram_size - BZ_KERNEL_START,
			.type		= E820_RAM,
		};
	} else {
		mem_map[i++]	= (struct e820entry) {
			.addr		= BZ_KERNEL_START,
			.size		= KVM_32BIT_GAP_START - BZ_KERNEL_START,
			.type		= E820_RAM,
		};
		mem_map[i++]	= (struct e820entry) {
			.addr		= KVM_32BIT_MAX_MEM_SIZE,
			.size		= kvm->ram_size - KVM_32BIT_MAX_MEM_SIZE,
			.type		= E820_RAM,
		};
	}

	BUG_ON(i > E820_X_MAX);

	e820->nr_map = i;
}

static void setup_vga_rom(struct kvm *kvm)
{
	u16 *mode;
	void *p;

	p = guest_flat_to_host(kvm, VGA_ROM_OEM_STRING);
	memset(p, 0, VGA_ROM_OEM_STRING_SIZE);
	strncpy(p, "KVM VESA", VGA_ROM_OEM_STRING_SIZE);

	mode = guest_flat_to_host(kvm, VGA_ROM_MODES);
	mode[0]	= 0x0112;
	mode[1] = 0xffff;
}

/**
 * setup_bios - inject BIOS into guest memory
 * @kvm - guest system descriptor
 */
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

	/* E820 memory map must be present */
	e820_setup(kvm);

	/* VESA needs own tricks */
	setup_vga_rom(kvm);

	/*
	 * Setup a *fake* real mode vector table, it has only
	 * one real handler which does just iret
	 */
	address = BIOS_IRQ_PA_ADDR(bios_intfake);
	intr_desc = (struct real_intr_desc) {
		.segment	= REAL_SEGMENT(MB_BIOS_BEGIN),
		.offset		= address - MB_BIOS_BEGIN,
	};
	interrupt_table__setup(&kvm->arch.interrupt_table, &intr_desc);

	for (i = 0; i < ARRAY_SIZE(bios_irq_handlers); i++)
		setup_irq_handler(kvm, &bios_irq_handlers[i]);

	/* we almost done */
	p = guest_flat_to_host(kvm, 0);
	interrupt_table__copy(&kvm->arch.interrupt_table, p, REAL_INTR_SIZE);
}
