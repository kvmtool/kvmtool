#ifndef KVM_E820_H
#define KVM_E820_H

#include <linux/types.h>

#define SMAP    0x534d4150      /* ASCII "SMAP" */

struct biosregs;

void e820_query_map(struct biosregs *regs);

#endif /* KVM_E820_H */
