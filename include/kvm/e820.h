#ifndef KVM_E820_H
#define KVM_E820_H

#include <linux/types.h>

#define SMAP    0x534d4150      /* ASCII "SMAP" */

#define E820_MEM_USABLE		1
#define E820_MEM_RESERVED	2

#define E820_MEM_AREAS		5

struct e820_entry {
	u64	addr;	/* start of memory segment */
	u64	size;	/* size of memory segment */
	u32	type;	/* type of memory segment */
} __attribute__((packed));

struct e820_query {
	u32	eax;
	u32	ebx;
	u32	edi;
	u32	ecx;
	u32	edx;
};

void e820_query_map(struct e820_query *query);

#endif /* KVM_E820_H */
