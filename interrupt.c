#include "kvm/interrupt.h"

#include "util.h"

#include <string.h>

static struct ivt_entry ivt_table[IVT_VECTORS];

void ivt_reset(void)
{
	memset(ivt_table, 0x0, sizeof(ivt_table));
}

void ivt_copy_table(void *dst, unsigned int size)
{
	if (size < sizeof(ivt_table))
		die("An attempt to overwrite host memory");
	memcpy(dst, ivt_table, sizeof(ivt_table));
}

struct ivt_entry * const ivt_get_entry(unsigned int n)
{
	struct ivt_entry *v = NULL;
	if (n < IVT_VECTORS)
		v = &ivt_table[n];
	return (struct ivt_entry * const)v;
}

void ivt_set_entry(struct ivt_entry e, unsigned int n)
{
	if (n < IVT_VECTORS)
		ivt_table[n] = e;
}

void ivt_set_all(struct ivt_entry e)
{
	unsigned int i;

	for (i = 0; i < IVT_VECTORS; i++)
		ivt_table[i] = e;
}
