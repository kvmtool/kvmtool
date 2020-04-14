#ifndef KVM__VESA_H
#define KVM__VESA_H

#define VESA_WIDTH	640
#define VESA_HEIGHT	480

#define VESA_MEM_ADDR	0xd0000000
#define VESA_BPP	32
/*
 * We actually only need VESA_BPP/8*VESA_WIDTH*VESA_HEIGHT bytes. But the memory
 * size must be a power of 2, so we round up.
 */
#define VESA_MEM_SIZE	(1 << 21)

struct kvm;
struct biosregs;

struct framebuffer *vesa__init(struct kvm *self);

#endif
