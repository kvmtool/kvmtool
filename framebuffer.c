#include "kvm/framebuffer.h"
#include "kvm/kvm.h"

#include <linux/kernel.h>
#include <linux/list.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>

static LIST_HEAD(framebuffers);

struct framebuffer *fb__register(struct framebuffer *fb)
{
	INIT_LIST_HEAD(&fb->node);
	list_add(&fb->node, &framebuffers);

	return fb;
}

int fb__attach(struct framebuffer *fb, struct fb_target_operations *ops)
{
	if (fb->nr_targets >= FB_MAX_TARGETS)
		return -ENOSPC;

	fb->targets[fb->nr_targets++] = ops;

	return 0;
}

static int start_targets(struct framebuffer *fb)
{
	unsigned long i;

	for (i = 0; i < fb->nr_targets; i++) {
		struct fb_target_operations *ops = fb->targets[i];
		int err = 0;

		if (ops->start)
			err = ops->start(fb);

		if (err)
			return err;
	}

	return 0;
}

int fb__init(struct kvm *kvm)
{
	struct framebuffer *fb;

	list_for_each_entry(fb, &framebuffers, node) {
		int err;

		err = start_targets(fb);
		if (err)
			return err;
	}

	return 0;
}
firmware_init(fb__init);

int fb__exit(struct kvm *kvm)
{
	struct framebuffer *fb;

	list_for_each_entry(fb, &framebuffers, node) {
		u32 i;

		for (i = 0; i < fb->nr_targets; i++)
			if (fb->targets[i]->stop)
				fb->targets[i]->stop(fb);

		munmap(fb->mem, fb->mem_size);
	}

	return 0;
}
firmware_exit(fb__exit);
