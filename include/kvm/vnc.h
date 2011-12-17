#ifndef KVM__VNC_H
#define KVM__VNC_H

struct framebuffer;

#ifdef CONFIG_HAS_VNCSERVER
int vnc__init(struct framebuffer *fb);
int vnc__exit(struct framebuffer *fb);
#else
static inline int vnc__init(struct framebuffer *fb)
{
	return 0;
}
static inline int vnc__exit(struct framebuffer *fb)
{
	return 0;
}
#endif

#endif /* KVM__VNC_H */
