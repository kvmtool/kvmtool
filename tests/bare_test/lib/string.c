#include <string.h>

size_t strlen(const char *str)
{
    size_t i = 0;
    for (; str[i] != '\0'; ++i)
        ;
    return i;
}

void *memset(void *src, char ch, size_t cnt)
{
    for (size_t i = 0; i < cnt; ++i)
        *((char *)src + i) = ch;
    return src;
}
