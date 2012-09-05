#ifndef KVM__PCKBD_H
#define KVM__PCKBD_H

#include <linux/types.h>

struct kvm;

void mouse_queue(u8 c);
void kbd_queue(u8 c);
int kbd__init(struct kvm *kvm);

#endif
