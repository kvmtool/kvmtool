#ifndef KVM_E820_H
#define KVM_E820_H

#include <linux/types.h>

#define SMAP    0x534d4150      /* ASCII "SMAP" */

struct e820_query {
	u32	eax;
	u32	ebx;
	u32	edi;
	u32	ecx;
	u32	edx;
};

void e820_query_map(struct e820_query *query);

#endif /* KVM_E820_H */
