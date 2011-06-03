#ifndef KVM__VNC_H
#define KVM__VNC_H

struct framebuffer;

#ifdef CONFIG_HAS_VNCSERVER
void vnc__init(struct framebuffer *fb);
#else
static inline void vnc__init(struct framebuffer *fb)
{
}
#endif

#endif /* KVM__VNC_H */
