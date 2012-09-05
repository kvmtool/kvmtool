#ifndef KVM__VNC_H
#define KVM__VNC_H

#include "kvm/kvm.h"

struct framebuffer;

#ifdef CONFIG_HAS_VNCSERVER
int vnc__init(struct kvm *kvm);
int vnc__exit(struct kvm *kvm);
#else
static inline int vnc__init(struct kvm *kvm)
{
	return 0;
}
static inline int vnc__exit(struct kvm *kvm)
{
	return 0;
}
#endif

#endif /* KVM__VNC_H */
