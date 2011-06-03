#ifndef KVM__PCKBD_H
#define KVM__PCKBD_H

struct kvm;

void kbd__init(struct kvm *kvm);

#ifdef CONFIG_HAS_VNCSERVER
#include <rfb/keysym.h>
#include <rfb/rfb.h>

void kbd_handle_key(rfbBool, rfbKeySym, rfbClientPtr);
void kbd_handle_ptr(int, int, int, rfbClientPtr);

#else

void kbd__init(struct kvm *kvm) { }

#endif

#endif
