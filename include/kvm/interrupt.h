#ifndef KVM__INTERRUPT_H
#define KVM__INTERRUPT_H

#include <inttypes.h>
#include "kvm/bios.h"

struct real_intr_desc {
	uint16_t offset;
	uint16_t segment;
} __attribute__((packed));

#define REAL_SEGMENT_SHIFT	4
#define REAL_SEGMENT(addr)	((addr) >> REAL_SEGMENT_SHIFT)
#define REAL_INTR_SIZE		(REAL_INTR_VECTORS * sizeof(struct real_intr_desc))

struct interrupt_table {
	struct real_intr_desc entries[REAL_INTR_VECTORS];
};

void interrupt_table__copy(struct interrupt_table *self, void *dst, unsigned int size);
void interrupt_table__setup(struct interrupt_table *self, struct real_intr_desc *entry);
void interrupt_table__set(struct interrupt_table *self, struct real_intr_desc *entry, unsigned int num);

/*
 * BIOS stubs
 */
extern unsigned char intfake[];
extern unsigned int intfake_size;
extern unsigned char int10[];
extern unsigned int int10_size;

#endif /* KVM__INTERRUPT_H */
