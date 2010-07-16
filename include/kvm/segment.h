#ifndef KVM_SEGMENT_H
#define KVM_SEGMENT_H

#include <stdint.h>

static inline uint16_t flat_to_seg16(uint32_t address)
{
	return address >> 4;
}

static inline uint16_t flat_to_off16(uint32_t address, uint32_t segment)
{
	return address - (segment << 4);
}

#endif /* KVM_SEGMENT_H */
