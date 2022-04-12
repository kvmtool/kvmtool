#ifndef KVM__BITMAP_H
#define KVM__BITMAP_H

#include <stdbool.h>
#include <string.h>

#include "linux/bitops.h"

#define DECLARE_BITMAP(name,bits) \
	unsigned long name[BITS_TO_LONGS(bits)]

#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
	memset(dst, 0, len);
}

#if __BYTE_ORDER__ ==  __ORDER_LITTLE_ENDIAN__
#define BITMAP_MEM_ALIGNMENT 8
#else
#define BITMAP_MEM_ALIGNMENT (8 * sizeof(unsigned long))
#endif
#define BITMAP_MEM_MASK (BITMAP_MEM_ALIGNMENT - 1)

void __bitmap_set(unsigned long *map, unsigned int start, int len);

static inline void bitmap_set(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		set_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
		 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0xff, nbits / 8);
	else
		__bitmap_set(map, start, nbits);
}

bool __bitmap_and(unsigned long *dst, const unsigned long *src1,
		  const unsigned long *src2, unsigned int nbits);

static inline bool bitmap_and(unsigned long *dst, const unsigned long *src1,
			      const unsigned long *src2, unsigned int nbits)
{
	if (nbits >= 0 && nbits <= BITS_PER_LONG)
		return (*dst = *src1 & *src2 & BITMAP_LAST_WORD_MASK(nbits)) != 0;

	return __bitmap_and(dst, src1, src2, nbits);
}

int bitmap_parselist(const char *buf, unsigned long *maskp, int nmaskbits);

bool __bitmap_subset(const unsigned long *bitmap1, const unsigned long *bitmap2,
		     unsigned int nbits);

static inline bool bitmap_subset(const unsigned long *src1,
				 const unsigned long *src2, unsigned int nbits)
{
	if (nbits >= 0 && nbits <= BITS_PER_LONG)
		return !((*src1 & ~(*src2)) & BITMAP_LAST_WORD_MASK(nbits));

	return __bitmap_subset(src1, src2, nbits);
}


#endif /* KVM__BITMAP_H */
