#ifndef KVM_BIOS_MEMCPY_H
#define KVM_BIOS_MEMCPY_H

#include <linux/types.h>
#include <stddef.h>

void memcpy16(u16 dst_seg, void *dst, u16 src_seg, const void *src, size_t len);

#endif /* KVM_BIOS_MEMCPY_H */
