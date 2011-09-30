#ifndef KVM_SEGMENT_H
#define KVM_SEGMENT_H

#include <linux/types.h>

static inline u32 segment_to_flat(u16 selector, u16 offset)
{
	return ((u32)selector << 4) + (u32) offset;
}

static inline u16 flat_to_seg16(u32 address)
{
	return address >> 4;
}

static inline u16 flat_to_off16(u32 address, u32 segment)
{
	return address - (segment << 4);
}

#endif /* KVM_SEGMENT_H */
