#ifndef KVM__VESA_H
#define KVM__VESA_H

#include <linux/types.h>

#define VESA_WIDTH	640
#define VESA_HEIGHT	480

#define VESA_MEM_ADDR	0xd0000000
#define VESA_MEM_SIZE	(4*VESA_WIDTH*VESA_HEIGHT)
#define VESA_BPP	32

struct kvm;
struct int10_args;

void vesa_mmio_callback(u64, u8*, u32, u8);
void vesa__init(struct kvm *self);
void *vesa__dovnc(void *);
void int10_handler(struct int10_args *args);

#ifndef CONFIG_HAS_VNCSERVER
void vesa__init(struct kvm *self) { }
#endif

extern u8 videomem[VESA_MEM_SIZE];

#endif
