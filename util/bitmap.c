/*
 * Taken from Linux kernel version v5.15.
 */
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <strings.h>

#include "linux/bitmap.h"
#include "linux/bitops.h"
#include "linux/err.h"

/*
 * Region 9-38:4/10 describes the following bitmap structure:
 * 0	   9  12    18			38	     N
 * .........****......****......****..................
 *	    ^  ^     ^			 ^	     ^
 *      start  off   group_len	       end	 nbits
 */
struct region {
	unsigned int start;
	unsigned int off;
	unsigned int group_len;
	unsigned int end;
	unsigned int nbits;
};

void __bitmap_set(unsigned long *map, unsigned int start, int len)
{
	unsigned long *p = map + BIT_WORD(start);
	const unsigned int size = start + len;
	int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_set = BITMAP_FIRST_WORD_MASK(start);

	while (len - bits_to_set >= 0) {
		*p |= mask_to_set;
		len -= bits_to_set;
		bits_to_set = BITS_PER_LONG;
		mask_to_set = ~0UL;
		p++;
	}
	if (len) {
		mask_to_set &= BITMAP_LAST_WORD_MASK(size);
		*p |= mask_to_set;
	}
}

static void bitmap_set_region(const struct region *r, unsigned long *bitmap)
{
	unsigned int start;

	for (start = r->start; start <= r->end; start += r->group_len)
		bitmap_set(bitmap, start, min(r->end - start + 1, r->off));
}

static inline bool __end_of_region(char c)
{
	return isspace(c) || c == ',';
}

static inline bool end_of_str(char c)
{
	return c == '\0' || c == '\n';
}

static inline bool end_of_region(char c)
{
	return __end_of_region(c) || end_of_str(c);
}

/*
 * The format allows commas and whitespaces at the beginning
 * of the region.
 */
static const char *bitmap_find_region(const char *str)
{
	while (__end_of_region(*str))
		str++;

	return end_of_str(*str) ? NULL : str;
}

static int bitmap_check_region(const struct region *r)
{
	if (r->start > r->end || r->group_len == 0 || r->off > r->group_len)
		return -EINVAL;

	if (r->end >= r->nbits)
		return -ERANGE;

	return 0;
}

static const char *bitmap_getnum(const char *str, unsigned int *num,
				 unsigned int lastbit)
{
	unsigned long long n;
	char *endptr;

	if (str[0] == 'N') {
		*num = lastbit;
		return str + 1;
	}

	n = strtoll(str, &endptr, 10);
	/* No digits found. */
	if (n == 0 && endptr == str)
		return ERR_PTR(-EINVAL);
	/* Check for overflows and negative numbers. */
	if (n == ULLONG_MAX || n != (unsigned long)n || n != (unsigned int)n)
		return ERR_PTR(-EOVERFLOW);

	*num = n;
	return endptr;
}

static const char *bitmap_parse_region(const char *str, struct region *r)
{
	unsigned int lastbit = r->nbits - 1;

	if (!strncasecmp(str, "all", 3)) {
		r->start = 0;
		r->end = lastbit;
		str += 3;

		goto check_pattern;
	}

	str = bitmap_getnum(str, &r->start, lastbit);
	if (IS_ERR(str))
		return str;

	if (end_of_region(*str))
		goto no_end;

	if (*str != '-')
		return ERR_PTR(-EINVAL);

	str = bitmap_getnum(str + 1, &r->end, lastbit);
	if (IS_ERR(str))
		return str;

check_pattern:
	if (end_of_region(*str))
		goto no_pattern;

	if (*str != ':')
		return ERR_PTR(-EINVAL);

	str = bitmap_getnum(str + 1, &r->off, lastbit);
	if (IS_ERR(str))
		return str;

	if (*str != '/')
		return ERR_PTR(-EINVAL);

	return bitmap_getnum(str + 1, &r->group_len, lastbit);

no_end:
	r->end = r->start;
no_pattern:
	r->off = r->end + 1;
	r->group_len = r->end + 1;

	return end_of_str(*str) ? NULL : str;
}

/**
 * bitmap_parselist - convert list format ASCII string to bitmap
 * @buf: read user string from this buffer; must be terminated
 *    with a \0 or \n.
 * @maskp: write resulting mask here
 * @nmaskbits: number of bits in mask to be written
 *
 * Input format is a comma-separated list of decimal numbers and
 * ranges.  Consecutively set bits are shown as two hyphen-separated
 * decimal numbers, the smallest and largest bit numbers set in
 * the range.
 * Optionally each range can be postfixed to denote that only parts of it
 * should be set. The range will divided to groups of specific size.
 * From each group will be used only defined amount of bits.
 * Syntax: range:used_size/group_size
 * Example: 0-1023:2/256 ==> 0,1,256,257,512,513,768,769
 * The value 'N' can be used as a dynamically substituted token for the
 * maximum allowed value; i.e (nmaskbits - 1).  Keep in mind that it is
 * dynamic, so if system changes cause the bitmap width to change, such
 * as more cores in a CPU list, then any ranges using N will also change.
 *
 * Returns: 0 on success, -errno on invalid input strings. Error values:
 *
 *   - ``-EINVAL``: wrong region format
 *   - ``-EINVAL``: invalid character in string
 *   - ``-ERANGE``: bit number specified too large for mask
 *   - ``-EOVERFLOW``: integer overflow in the input parameters
 */
int bitmap_parselist(const char *buf, unsigned long *maskp, int nmaskbits)
{
	struct region r;
	long ret;

	r.nbits = nmaskbits;
	bitmap_zero(maskp, r.nbits);

	while (buf) {
		buf = bitmap_find_region(buf);
		if (buf == NULL)
			return 0;

		buf = bitmap_parse_region(buf, &r);
		if (IS_ERR(buf))
			return PTR_ERR(buf);

		ret = bitmap_check_region(&r);
		if (ret)
			return ret;

		bitmap_set_region(&r, maskp);
	}

	return 0;
}

bool __bitmap_and(unsigned long *dst, const unsigned long *src1,
		  const unsigned long *src2, unsigned int nbits)
{
	unsigned int lim = nbits / BITS_PER_LONG;
	unsigned long result = 0;
	unsigned int k;

	for (k = 0; k < lim; k++)
		result |= (dst[k] = src1[k] & src2[k]);

	if (nbits % BITS_PER_LONG) {
		result |= (dst[k] = src1[k] & src2[k] &
			   BITMAP_LAST_WORD_MASK(nbits));
	}

	return result != 0;
}

bool __bitmap_subset(const unsigned long *bitmap1, const unsigned long *bitmap2,
		     unsigned int nbits)
{
	unsigned int k, lim = nbits / BITS_PER_LONG;

	for (k = 0; k < lim; k++)
		if (bitmap1[k] & ~bitmap2[k])
			return false;

	if (nbits % BITS_PER_LONG) {
		if ((bitmap1[k] & ~bitmap2[k]) & BITMAP_LAST_WORD_MASK(nbits))
			return false;
	}

	return true;
}
