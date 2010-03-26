#ifndef UTIL_H_
#define UTIL_H_

/*
 * Some bits are stolen from perf tool :)
 */

#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
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

#define __stringify_1(x)	#x
#define __stringify(x)		__stringify_1(x)

extern void die(const char *err, ...) NORETURN __attribute__((format (printf, 1, 2)));
extern void die_perror(const char *s) NORETURN;
extern int error(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void warning(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void info(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void set_die_routine(void (*routine)(const char *err, va_list params) NORETURN);

#define DIE_IF(cnd)						\
do {								\
	if (cnd)						\
	die(" at (" __FILE__ ":" __stringify(__LINE__) "): "	\
		__stringify(cnd) "\n");				\
} while (0)

#endif /* UTIL_H_ */
