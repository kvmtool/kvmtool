#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

#include <stdint.h>

#define __s8		int8_t
#define __u8		uint8_t

#define __s16		int16_t
#define __u16		uint16_t

#define __s32		int32_t
#define __u32		uint32_t

#define __s64		long long
#define __u64		unsigned long long

typedef __u64 u64;
typedef __s64 s64;

typedef __u32 u32;
typedef __s32 s32;

typedef __u16 u16;
typedef __s16 s16;

typedef __u8  u8;
typedef __s8  s8;

#endif /* LINUX_TYPES_H */
