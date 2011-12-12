
/* user defined headers */
#include <kvm/util.h>
#include <kvm/strbuf.h>

int prefixcmp(const char *str, const char *prefix)
{
	for (; ; str++, prefix++) {
		if (!*prefix)
			return 0;
		else if (*str != *prefix)
			return (unsigned char)*prefix - (unsigned char)*str;
	}
}

/**
 * strlcat - Append a length-limited, %NUL-terminated string to another
 * @dest: The string to be appended to
 * @src: The string to append to it
 * @count: The size of the destination buffer.
 */
size_t strlcat(char *dest, const char *src, size_t count)
{
	size_t dsize = strlen(dest);
	size_t len = strlen(src);
	size_t res = dsize + len;

	DIE_IF(dsize >= count);

	dest += dsize;
	count -= dsize;
	if (len >= count)
		len = count - 1;

	memcpy(dest, src, len);
	dest[len] = 0;

	return res;
}
