#ifndef LINUX__BITS_H_
#define LINUX__BITS_H_

#define GENMASK(h, l) \
	((~0UL - (1UL << (l)) + 1) & \
	 (~0UL >> (BITS_PER_LONG - 1 - (h))))

#endif /* LINUX__BITS_H */
