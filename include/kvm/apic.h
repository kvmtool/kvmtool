#ifndef KVM_APIC_H_
#define KVM_APIC_H_

#include <asm/apicdef.h>

/*
 * APIC, IOAPIC stuff
 */
#define APIC_BASE_ADDR_STEP	0x00400000
#define IOAPIC_BASE_ADDR_STEP	0x00100000

#define APIC_ADDR(apic)		(APIC_DEFAULT_PHYS_BASE + apic * APIC_BASE_ADDR_STEP)
#define IOAPIC_ADDR(ioapic)	(IO_APIC_DEFAULT_PHYS_BASE + ioapic * IOAPIC_BASE_ADDR_STEP)

#define KVM_APIC_VERSION	0x14 /* xAPIC */

#endif /* KVM_APIC_H_ */
