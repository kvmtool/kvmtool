#ifndef KVM__INTERRUPT_H
#define KVM__INTERRUPT_H

#include <inttypes.h>

#define IVT_BASE	0x0000
#define IVT_VECTORS	256

struct ivt_entry {
	uint16_t offset;
	uint16_t segment;
} __attribute__((packed));

struct interrupt_table {
	struct ivt_entry entries[IVT_VECTORS];
};

void interrupt_table__copy(struct interrupt_table *self, void *dst, unsigned int size);
void interrupt_table__setup(struct interrupt_table *self, struct ivt_entry *entry);

#endif /* KVM__INTERRUPT_H */
