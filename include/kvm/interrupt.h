#ifndef KVM__INTERRUPT_H
#define KVM__INTERRUPT_H

#include <inttypes.h>

/*
 * BIOS data area
 */
#define BDA_START		0xf0000
#define BDA_END			0xfffff
#define BDA_SIZE		(BDA_END - BDA_START)

#define REAL_INTR_BASE		0x0000
#define REAL_INTR_VECTORS	256

struct real_intr_desc {
	uint16_t offset;
	uint16_t segment;
} __attribute__((packed));

#define REAL_INTR_SIZE		(REAL_INTR_VECTORS * sizeof(struct real_intr_desc))

struct interrupt_table {
	struct real_intr_desc entries[REAL_INTR_VECTORS];
};

void interrupt_table__copy(struct interrupt_table *self, void *dst, unsigned int size);
void interrupt_table__setup(struct interrupt_table *self, struct real_intr_desc *entry);

#endif /* KVM__INTERRUPT_H */
