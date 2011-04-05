#ifndef KVM_E820_H
#define KVM_E820_H

#include <stdint.h>

#define SMAP    0x534d4150      /* ASCII "SMAP" */

#define E820_MEM_USABLE		1
#define E820_MEM_RESERVED	2

#define E820_MEM_AREAS		4

struct e820_entry {
	uint64_t addr;	/* start of memory segment */
	uint64_t size;	/* size of memory segment */
	uint32_t type;	/* type of memory segment */
} __attribute__((packed));

struct e820_query {
	uint32_t	eax;
	uint32_t	ebx;
	uint32_t	edi;
	uint32_t	ecx;
	uint32_t	edx;
};

void e820_query_map(struct e820_query *query);

#endif /* KVM_E820_H */
