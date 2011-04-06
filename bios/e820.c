#include "kvm/e820.h"

#include "kvm/segment.h"
#include "kvm/bios.h"
#include "kvm/util.h"

static inline void set_fs(uint16_t seg)
{
	asm volatile("movw %0,%%fs" : : "rm" (seg));
}

static inline uint8_t rdfs8(unsigned long addr)
{
	uint8_t v;

	asm volatile("addr32 movb %%fs:%1,%0" : "=q" (v) : "m" (*(uint8_t *)addr));

	return v;
}

bioscall void e820_query_map(struct e820_query *query)
{
	uint8_t map_size;
	uint16_t fs_seg;
	uint32_t ndx;

	fs_seg		= flat_to_seg16(E820_MAP_SIZE);
	set_fs(fs_seg);

	ndx		= query->ebx;

	map_size	= rdfs8(flat_to_off16(E820_MAP_SIZE, fs_seg));

	if (ndx < map_size) {
		unsigned long start;
		unsigned int i;
		uint8_t *p;

		fs_seg		= flat_to_seg16(E820_MAP_START);
		set_fs(fs_seg);

		start	= E820_MAP_START + sizeof(struct e820_entry) * ndx;

		p	= (void *) query->edi;

		for (i = 0; i < sizeof(struct e820_entry); i++)
			*p++	= rdfs8(flat_to_off16(start + i, fs_seg));
	}

	query->eax	= SMAP;
	query->ecx	= sizeof(struct e820_entry);
	query->ebx	= ++ndx;

	if (ndx >= map_size)
		query->ebx	= 0;	/* end of map */
}
