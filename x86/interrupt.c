#include "kvm/interrupt.h"

#include "kvm/util.h"

#include <string.h>

void interrupt_table__copy(struct interrupt_table *itable, void *dst, unsigned int size)
{
	if (size < sizeof(itable->entries))
		die("An attempt to overwrite host memory");

	memcpy(dst, itable->entries, sizeof(itable->entries));
}

void interrupt_table__setup(struct interrupt_table *itable, struct real_intr_desc *entry)
{
	unsigned int i;

	for (i = 0; i < REAL_INTR_VECTORS; i++)
		itable->entries[i] = *entry;
}

void interrupt_table__set(struct interrupt_table *itable,
				struct real_intr_desc *entry, unsigned int num)
{
	if (num < REAL_INTR_VECTORS)
		itable->entries[num] = *entry;
}
