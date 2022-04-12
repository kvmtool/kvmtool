#ifndef LINUX__CPUMASK_H
#define LINUX__CPUMASK_H

#include <stdbool.h>

#include "linux/bitmap.h"
#include "linux/find.h"
#include "linux/kernel.h"

typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;

#define cpumask_bits(maskp)	((maskp)->bits)

static inline unsigned int cpumask_size(void)
{
	return BITS_TO_LONGS(NR_CPUS) * sizeof(long);
}

static inline void cpumask_set_cpu(int cpu, cpumask_t *dstp)
{
	set_bit(cpu, cpumask_bits(dstp));
}

static inline void cpumask_clear_cpu(int cpu, cpumask_t *dstp)
{
	clear_bit(cpu, cpumask_bits(dstp));
}

static inline bool cpumask_test_cpu(int cpu, const cpumask_t *cpumask)
{
	return test_bit(cpu, cpumask_bits((cpumask)));
}

static inline void cpumask_clear(cpumask_t *dstp)
{
	bitmap_zero(cpumask_bits(dstp), NR_CPUS);
}

static inline bool cpumask_and(cpumask_t *dstp, cpumask_t *src1p,
			       cpumask_t *src2p)
{
	return bitmap_and(cpumask_bits(dstp), cpumask_bits(src1p),
			  cpumask_bits(src2p), NR_CPUS);
}

static inline unsigned int cpumask_next(int n, const struct cpumask *srcp)
{
	return find_next_bit(cpumask_bits(srcp), NR_CPUS, n + 1);
}

#define for_each_cpu(cpu, maskp)			\
	for ((cpu) = -1;				\
	     (cpu) = cpumask_next((cpu), (maskp)),	\
	     (cpu) < NR_CPUS;)

static inline int cpulist_parse(const char *buf, cpumask_t *dstp)
{
	return bitmap_parselist(buf, cpumask_bits(dstp), NR_CPUS);
}

static inline bool cpumask_subset(const  cpumask_t *src1p,
				  const  cpumask_t *src2p)
{
	return bitmap_subset(cpumask_bits(src1p), cpumask_bits(src2p), NR_CPUS);
}

#endif /* LINUX__CPUMASK_H */
