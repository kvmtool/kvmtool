
/* user defined headers */
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
