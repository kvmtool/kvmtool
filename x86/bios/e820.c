#include "kvm/e820.h"

#include "kvm/segment.h"
#include "kvm/bios.h"

#include <asm/processor-flags.h>
#include <asm/e820.h>

static inline void set_fs(u16 seg)
{
	asm volatile("movw %0,%%fs" : : "rm" (seg));
}

static inline u8 rdfs8(unsigned long addr)
{
	u8 v;

	asm volatile("addr32 movb %%fs:%1,%0" : "=q" (v) : "m" (*(u8 *)addr));

	return v;
}

static inline u32 rdfs32(unsigned long addr)
{
	u32 v;

	asm volatile("addr32 movl %%fs:%1,%0" : "=q" (v) : "m" (*(u32 *)addr));

	return v;
}

bioscall void e820_query_map(struct biosregs *regs)
{
	struct e820map *e820;
	u32 map_size;
	u16 fs_seg;
	u32 ndx;

	e820		= (struct e820map *)E820_MAP_START;
	fs_seg		= flat_to_seg16(E820_MAP_START);
	set_fs(fs_seg);

	ndx		= regs->ebx;

	map_size	= rdfs32(flat_to_off16((u32)&e820->nr_map, fs_seg));

	if (ndx < map_size) {
		u32 start;
		unsigned int i;
		u8 *p;

		fs_seg	= flat_to_seg16(E820_MAP_START);
		set_fs(fs_seg);

		start	= (u32)&e820->map[ndx];

		p	= (void *) regs->edi;

		for (i = 0; i < sizeof(struct e820entry); i++)
			*p++	= rdfs8(flat_to_off16(start + i, fs_seg));
	}

	regs->eax	= SMAP;
	regs->ecx	= sizeof(struct e820entry);
	regs->ebx	= ++ndx;

	/* Clear CF to indicate success.  */
	regs->eflags	&= ~X86_EFLAGS_CF;

	if (ndx >= map_size)
		regs->ebx	= 0;	/* end of map */
}
