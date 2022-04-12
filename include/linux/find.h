#ifndef LINUX__FIND_H
#define LINUX__FIND_H

#include <stddef.h>

#include "linux/bitops.h"
#include "linux/bits.h"

unsigned long _find_next_bit(const unsigned long *addr1,
		const unsigned long *addr2, unsigned long nbits,
		unsigned long start, unsigned long invert);

static inline
unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
			    unsigned long offset)
{
	if (size >= 0 && size <= BITS_PER_LONG) {
		unsigned long val;

		if (offset >= size)
			return size;

		val = *addr & GENMASK(size - 1, offset);
		return val ? (unsigned long)__builtin_ctzl(val) : size;
	}

	return _find_next_bit(addr, NULL, size, offset, 0);
}

#endif /* LINUX__FIND_H */
