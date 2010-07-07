#ifndef BIOS_H_
#define BIOS_H_

/*
 * Motherboard BIOS
 */
#define KVM_BIOS_START		0xf0000
#define KVM_BIOS_END		0xfffff

#define REAL_INTR_BASE		0x0000
#define REAL_INTR_VECTORS	256

#define ALIGN(x, a)		(((x)+((a)-1))&~((a)-1))

#endif /* BIOS_H_ */
