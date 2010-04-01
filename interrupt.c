#include "kvm/interrupt.h"

#include "kvm/util.h"

#include <string.h>

void interrupt_table__copy(struct interrupt_table *self, void *dst, unsigned int size)
{
	if (size < sizeof(self->entries))
		die("An attempt to overwrite host memory");

	memcpy(dst, self->entries, sizeof(self->entries));
}

void interrupt_table__setup(struct interrupt_table *self, struct real_intr_desc *entry)
{
	unsigned int i;

	for (i = 0; i < REAL_INTR_VECTORS; i++)
		self->entries[i] = *entry;
}

void interrupt_table__set(struct interrupt_table *self, struct real_intr_desc *entry, unsigned int num)
{
	if (num < REAL_INTR_VECTORS)
		self->entries[num] = *entry;
}
