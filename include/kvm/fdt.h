#ifndef KVM__FDT_H
#define KVM__FDT_H

#include "libfdt.h"

#include <linux/types.h>

#define FDT_MAX_SIZE	0x10000

/* Helper for the various bits of code that generate FDT nodes */
#define _FDT(exp)							\
	do {								\
		int ret = (exp);					\
		if (ret < 0) {						\
			die("Error creating device tree: %s: %s\n",	\
			    #exp, fdt_strerror(ret));			\
		}							\
	} while (0)

static inline u32 fdt__alloc_phandle(void)
{
	static u32 phandle = 0;
	return ++phandle;
}

#endif /* KVM__FDT_H */
