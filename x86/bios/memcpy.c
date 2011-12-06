#include "bios/memcpy.h"

/*
 *  Copy memory area in 16-bit real mode.
 */
void memcpy16(u16 dst_seg, void *dst, u16 src_seg, const void *src, size_t len)
{
	__asm__ __volatile__ (
		"pushw	%%ds				\n"
		"pushw	%%es				\n"
		"movw	%[src_seg], %%ds		\n"
		"movw	%[dst_seg], %%es		\n"
		"rep movsb %%ds:(%%si), %%es:(%%di)	\n"
		"popw	%%es				\n"
		"popw	%%ds				\n"
		:
		: "S"(src),
		  "D"(dst),
		  "c"(len),
		  [src_seg] "r"(src_seg),
		  [dst_seg] "r"(dst_seg)
		: "cc", "memory");
}
