#ifndef KVM__VESA_H
#define KVM__VESA_H

#include <linux/types.h>

#define VESA_WIDTH	640
#define VESA_HEIGHT	480

#define VESA_MEM_ADDR	0xd0000000
#define VESA_MEM_SIZE	(4*VESA_WIDTH*VESA_HEIGHT)
#define VESA_BPP	32

struct kvm;
struct biosregs;

struct framebuffer *vesa__init(struct kvm *self);

#endif
