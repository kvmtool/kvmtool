#ifndef _PERF_LINUX_COMPILER_H_
#define _PERF_LINUX_COMPILER_H_

#ifndef __always_inline
#define __always_inline	inline
#endif
#define __user

#ifndef __attribute_const__
#define __attribute_const__
#endif

#define __used		__attribute__((__unused__))
#define __packed	__attribute__((packed))
#define __iomem
#define __force
#define __must_check
#define unlikely

#endif
