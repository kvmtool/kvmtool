#ifndef KVM__FRAMEBUFFER_H
#define KVM__FRAMEBUFFER_H

#include <linux/types.h>
#include <linux/list.h>

struct framebuffer;

struct fb_target_operations {
	int (*start)(struct framebuffer *fb);
	int (*stop)(struct framebuffer *fb);
};

#define FB_MAX_TARGETS			2

struct framebuffer {
	struct list_head		node;

	u32				width;
	u32				height;
	u8				depth;
	char				*mem;
	u64				mem_addr;
	u64				mem_size;
	struct kvm			*kvm;

	unsigned long			nr_targets;
	struct fb_target_operations	*targets[FB_MAX_TARGETS];
};

struct framebuffer *fb__register(struct framebuffer *fb);
int fb__attach(struct framebuffer *fb, struct fb_target_operations *ops);
int fb__init(struct kvm *kvm);
int fb__exit(struct kvm *kvm);

#endif /* KVM__FRAMEBUFFER_H */
