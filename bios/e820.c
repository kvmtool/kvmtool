#include "kvm/e820.h"

#include "kvm/bios.h"
#include "kvm/util.h"

static inline void outb(uint8_t v, uint16_t port)
{
	asm volatile("outb %0,%1" : : "a" (v), "dN" (port));
}

static inline uint8_t rdfs8(unsigned long addr)
{
	uint8_t v;

	asm volatile("addr32 movb %%fs:%1,%0" : "=q" (v) : "m" (*(uint8_t *)addr));

	return v;
}

void e820_query_map(struct e820_query *query)
{
	uint8_t map_size;
	uint32_t ndx;

	ndx		= query->ebx;

	map_size	= rdfs8(E820_MAP_SIZE);

	if (ndx < map_size) {
		unsigned long start;
		unsigned int i;
		uint8_t *p;

		start	= E820_MAP_START + sizeof(struct e820_entry) * ndx;

		p	= (void *) query->edi;

		for (i = 0; i < sizeof(struct e820_entry); i++)
			*p++	= rdfs8(start + i);
	}

	query->eax	= SMAP;
	query->ecx	= 20;
	query->ebx	= ++ndx;

	if (ndx >= map_size)
		query->ebx	= 0;	/* end of map */
}
