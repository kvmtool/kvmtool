#ifndef __TIMER_H
#define __TIMER_H

#include "asm/kvm.h"
#include <time.h>
#include <stdbool.h>

#define HOST_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define NANOSECONDS_PER_SECOND 1000000000LL
#define MIN_TIMER_REARM_NS 250000

void cpu_enable_ticks(struct kvm *kvm);
void cpu_disable_ticks(struct kvm *kvm);

static inline u64 muldiv64_rounding(u64 a, u32 b, u32 c,
								  bool round_up)
{
	union {
		u64 ll;
		struct {
#if HOST_BIG_ENDIAN
			u32 high, low;
#else
			u32 low, high;
#endif
		} l;
	} u, res;
	u64 rl, rh;

	u.ll = a;
	rl = (u64)u.l.low * (u64)b;
	if (round_up) {
		rl += c - 1;
	}
	rh = (u64)u.l.high * (u64)b;
	rh += (rl >> 32);
	res.l.high = rh / c;
	res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
	return res.ll;
}

/* a*b/c */
static inline u64 muldiv64(u64 a, u32 b, u32 c)
{
	return muldiv64_rounding(a, b, c, false);
}

/* a*b/c round up */
static inline u64 muldiv64_round_up(u64 a, u32 b, u32 c)
{
	return muldiv64_rounding(a, b, c, true);
}

static inline s64 get_clock(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * NANOSECONDS_PER_SECOND + ts.tv_nsec;
}

#if defined(__i386__)

static inline s64 cpu_get_host_ticks(void)
{
	s64 val;
	asm volatile ("rdtsc" : "=A" (val));
	return val;
}

#elif defined(__x86_64__)
static inline s64 cpu_get_host_ticks(void)
{
	u32 low,high;
	s64 val;
	asm volatile("rdtsc" : "=a" (low), "=d" (high));
	val = high;
	val <<= 32;
	val |= low;
	return val;
}
#else
/* The host CPU doesn't have an easily accessible cycle counter.
   Just return a monotonically increasing value.  This will be
   totally wrong, but hopefully better than nothing.  */
static inline s64 cpu_get_host_ticks(void)
{
	return get_clock();
}
#endif

#endif