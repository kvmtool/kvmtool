#ifndef __STRBUF_H__
#define __STRBUF_H__

#include <sys/types.h>
#include <string.h>

int prefixcmp(const char *str, const char *prefix);

extern size_t strlcat(char *dest, const char *src, size_t count);
extern size_t strlcpy(char *dest, const char *src, size_t size);

/* some inline functions */

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

#endif
