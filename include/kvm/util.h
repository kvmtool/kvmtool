#include <linux/stringify.h>

#ifndef KVM__UTIL_H
#define KVM__UTIL_H

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/*
 * Some bits are stolen from perf tool :)
 */

#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>

#ifdef __GNUC__
#define NORETURN __attribute__((__noreturn__))
#else
#define NORETURN
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif

extern bool do_debug_print;

#define PROT_RW (PROT_READ|PROT_WRITE)
#define MAP_ANON_NORESERVE (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)

extern void die(const char *err, ...) NORETURN __attribute__((format (printf, 1, 2)));
extern void die_perror(const char *s) NORETURN;
extern int pr_error(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void pr_warning(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void pr_info(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void set_die_routine(void (*routine)(const char *err, va_list params) NORETURN);

#define pr_debug(fmt, ...)						\
	do {								\
		if (do_debug_print)					\
			pr_info("(%s) %s:%d: " fmt, __FILE__,		\
				__func__, __LINE__, ##__VA_ARGS__);	\
	} while (0)

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#define DIE_IF(cnd)						\
do {								\
	if (cnd)						\
	die(" at (" __FILE__ ":" __stringify(__LINE__) "): "	\
		__stringify(cnd) "\n");				\
} while (0)

extern size_t strlcat(char *dest, const char *src, size_t count);

/* some inline functions */

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

#define MSECS_TO_USECS(s) ((s) * 1000)

/* Millisecond sleep */
static inline void msleep(unsigned int msecs)
{
	usleep(MSECS_TO_USECS(msecs));
}
#endif /* KVM__UTIL_H */
