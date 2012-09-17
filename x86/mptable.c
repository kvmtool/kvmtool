#include "kvm/kvm.h"
#include "kvm/bios.h"
#include "kvm/apic.h"
#include "kvm/mptable.h"
#include "kvm/util.h"
#include "kvm/irq.h"

#include <linux/kernel.h>
#include <string.h>

#include <asm/mpspec_def.h>
#include <linux/types.h>

/*
 * FIXME: please make sure the addresses borrowed
 * for apic/ioapic never overlaped! We need a global
 * tracker of system resources (including io, mmio,
 * and friends).
 */

static unsigned int mpf_checksum(unsigned char *mp, int len)
{
	unsigned int sum = 0;

	while (len--)
		sum += *mp++;

	return sum & 0xFF;
}

static unsigned int gen_cpu_flag(unsigned int cpu, unsigned int ncpu)
{
	/* sets enabled/disabled | BSP/AP processor */
	return ( (cpu < ncpu) ? CPU_ENABLED       : 0) |
		((cpu == 0)   ? CPU_BOOTPROCESSOR : 0x00);
}

#define MPTABLE_SIG_FLOATING	"_MP_"
#define MPTABLE_OEM		"KVMCPU00"
#define MPTABLE_PRODUCTID	"0.1         "
#define MPTABLE_PCIBUSTYPE	"PCI   "
#define MPTABLE_ISABUSTYPE	"ISA   "

#define MPTABLE_STRNCPY(d, s)	memcpy(d, s, sizeof(d))

/* It should be more than enough */
#define MPTABLE_MAX_SIZE	(32 << 20)

/*
 * Too many cpus will require x2apic mode
 * and rather ACPI support so we limit it
 * here for a while.
 */
#define MPTABLE_MAX_CPUS	255

static void mptable_add_irq_src(struct mpc_intsrc *mpc_intsrc,
				u16 srcbusid,	u16 srcbusirq,
				u16 dstapic,	u16 dstirq)
{
	*mpc_intsrc = (struct mpc_intsrc) {
		.type		= MP_INTSRC,
		.irqtype	= mp_INT,
		.irqflag	= MP_IRQDIR_DEFAULT,
		.srcbus		= srcbusid,
		.srcbusirq	= srcbusirq,
		.dstapic	= dstapic,
		.dstirq		= dstirq
	};
}

/**
 * mptable_setup - create mptable and fill guest memory with it
 */
int mptable__init(struct kvm *kvm)
{
	unsigned long real_mpc_table, real_mpf_intel, size;
	struct mpf_intel *mpf_intel;
	struct mpc_table *mpc_table;
	struct mpc_cpu *mpc_cpu;
	struct mpc_bus *mpc_bus;
	struct mpc_ioapic *mpc_ioapic;
	struct mpc_intsrc *mpc_intsrc;
	struct rb_node *pci_tree;

	const int pcibusid = 0;
	const int isabusid = 1;

	unsigned int i, nentries = 0, ncpus = kvm->nrcpus;
	unsigned int ioapicid;
	void *last_addr;

	/* That is where MP table will be in guest memory */
	real_mpc_table = ALIGN(MB_BIOS_BEGIN + bios_rom_size, 16);

	if (ncpus > MPTABLE_MAX_CPUS) {
		pr_warning("Too many cpus: %d limited to %d",
			ncpus, MPTABLE_MAX_CPUS);
		ncpus = MPTABLE_MAX_CPUS;
	}

	mpc_table = calloc(1, MPTABLE_MAX_SIZE);
	if (!mpc_table)
		return -ENOMEM;

	MPTABLE_STRNCPY(mpc_table->signature,	MPC_SIGNATURE);
	MPTABLE_STRNCPY(mpc_table->oem,		MPTABLE_OEM);
	MPTABLE_STRNCPY(mpc_table->productid,	MPTABLE_PRODUCTID);

	mpc_table->spec		= 4;
	mpc_table->lapic	= APIC_ADDR(0);
	mpc_table->oemcount	= ncpus; /* will be updated again at end */

	/*
	 * CPUs enumeration. Technically speaking we should
	 * ask either host or HV for apic version supported
	 * but for a while we simply put some random value
	 * here.
	 */
	mpc_cpu = (void *)&mpc_table[1];
	for (i = 0; i < ncpus; i++) {
		mpc_cpu->type		= MP_PROCESSOR;
		mpc_cpu->apicid		= i;
		mpc_cpu->apicver	= KVM_APIC_VERSION;
		mpc_cpu->cpuflag	= gen_cpu_flag(i, ncpus);
		mpc_cpu->cpufeature	= 0x600; /* some default value */
		mpc_cpu->featureflag	= 0x201; /* some default value */
		mpc_cpu++;
	}

	last_addr = (void *)mpc_cpu;
	nentries += ncpus;

	/*
	 * PCI buses.
	 * FIXME: Some callback here to obtain real number
	 * of PCI buses present in system.
	 */
	mpc_bus		= last_addr;
	mpc_bus->type	= MP_BUS;
	mpc_bus->busid	= pcibusid;
	MPTABLE_STRNCPY(mpc_bus->bustype, MPTABLE_PCIBUSTYPE);

	last_addr = (void *)&mpc_bus[1];
	nentries++;

	/*
	 * ISA bus.
	 * FIXME: Same issue as for PCI bus.
	 */
	mpc_bus		= last_addr;
	mpc_bus->type	= MP_BUS;
	mpc_bus->busid	= isabusid;
	MPTABLE_STRNCPY(mpc_bus->bustype, MPTABLE_ISABUSTYPE);

	last_addr = (void *)&mpc_bus[1];
	nentries++;

	/*
	 * IO-APIC chip.
	 */
	ioapicid		= ncpus + 1;
	mpc_ioapic		= last_addr;
	mpc_ioapic->type	= MP_IOAPIC;
	mpc_ioapic->apicid	= ioapicid;
	mpc_ioapic->apicver	= KVM_APIC_VERSION;
	mpc_ioapic->flags	= MPC_APIC_USABLE;
	mpc_ioapic->apicaddr	= IOAPIC_ADDR(0);

	last_addr = (void *)&mpc_ioapic[1];
	nentries++;

	/*
	 * IRQ sources.
	 *
	 * FIXME: Same issue as with buses. We definitely
	 * need kind of collector routine which enumerate
	 * resources used first and pass them here.
	 * At moment we know we have only virtio block device
	 * and virtio console but this is g00berfish.
	 *
	 * Also note we use PCI irqs here, no for ISA bus yet.
	 */

	for (pci_tree = irq__get_pci_tree(); pci_tree; pci_tree = rb_next(pci_tree)) {
		struct pci_dev *dev = rb_entry(pci_tree, struct pci_dev, node);
		struct irq_line *irq_line;

		list_for_each_entry(irq_line, &dev->lines, node) {
			unsigned char srcbusirq;

			srcbusirq = (dev->id << 2) | (dev->pin - 1);

			mpc_intsrc = last_addr;

			mptable_add_irq_src(mpc_intsrc, pcibusid, srcbusirq, ioapicid, irq_line->line);
			last_addr = (void *)&mpc_intsrc[1];
			nentries++;
		}
	}

	/*
	 * Local IRQs assignment (LINT0, LINT1)
	 */
	mpc_intsrc		= last_addr;
	mpc_intsrc->type	= MP_LINTSRC;
	mpc_intsrc->irqtype	= mp_ExtINT;
	mpc_intsrc->irqtype	= mp_INT;
	mpc_intsrc->irqflag	= MP_IRQDIR_DEFAULT;
	mpc_intsrc->srcbus	= isabusid;
	mpc_intsrc->srcbusirq	= 0;
	mpc_intsrc->dstapic	= 0; /* FIXME: BSP apic */
	mpc_intsrc->dstirq	= 0; /* LINT0 */

	last_addr = (void *)&mpc_intsrc[1];
	nentries++;

	mpc_intsrc		= last_addr;
	mpc_intsrc->type	= MP_LINTSRC;
	mpc_intsrc->irqtype	= mp_NMI;
	mpc_intsrc->irqflag	= MP_IRQDIR_DEFAULT;
	mpc_intsrc->srcbus	= isabusid;
	mpc_intsrc->srcbusirq	= 0;
	mpc_intsrc->dstapic	= 0; /* FIXME: BSP apic */
	mpc_intsrc->dstirq	= 1; /* LINT1 */

	last_addr = (void *)&mpc_intsrc[1];
	nentries++;

	/*
	 * Floating MP table finally.
	 */
	real_mpf_intel	= ALIGN((unsigned long)last_addr - (unsigned long)mpc_table, 16);
	mpf_intel	= (void *)((unsigned long)mpc_table + real_mpf_intel);

	MPTABLE_STRNCPY(mpf_intel->signature, MPTABLE_SIG_FLOATING);
	mpf_intel->length	= 1;
	mpf_intel->specification= 4;
	mpf_intel->physptr	= (unsigned int)real_mpc_table;
	mpf_intel->checksum	= -mpf_checksum((unsigned char *)mpf_intel, sizeof(*mpf_intel));

	/*
	 * No last_addr inclrement here please, we need last
	 * active position here to compute table size.
	 */

	/*
	 * Don't forget to update header in fixed table.
	*/
	mpc_table->oemcount	= nentries;
	mpc_table->length	= last_addr - (void *)mpc_table;
	mpc_table->checksum	= -mpf_checksum((unsigned char *)mpc_table, mpc_table->length);


	/*
	 * We will copy the whole table, no need to separate
	 * floating structure and table itkvm.
	 */
	size = (unsigned long)mpf_intel + sizeof(*mpf_intel) - (unsigned long)mpc_table;

	/*
	 * The finial check -- never get out of system bios
	 * area. Lets also check for allocated memory overrun,
	 * in real it's late but still usefull.
	 */

	if (size > (unsigned long)(MB_BIOS_END - bios_rom_size) ||
	    size > MPTABLE_MAX_SIZE) {
		free(mpc_table);
		pr_err("MP table is too big");

		return -E2BIG;
	}

	/*
	 * OK, it is time to move it to guest memory.
	 */
	memcpy(guest_flat_to_host(kvm, real_mpc_table), mpc_table, size);

	free(mpc_table);

	return 0;
}
firmware_init(mptable__init);

int mptable__exit(struct kvm *kvm)
{
	return 0;
}
firmware_exit(mptable__exit);
