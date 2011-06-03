#include "kvm/vnc.h"

#include "kvm/framebuffer.h"
#include "kvm/i8042.h"

#include <linux/types.h>
#include <rfb/rfb.h>
#include <pthread.h>

#define VESA_QUEUE_SIZE		128
#define VESA_IRQ		14

/*
 * This "6000" value is pretty much the result of experimentation
 * It seems that around this value, things update pretty smoothly
 */
#define VESA_UPDATE_TIME	6000

static void vnc__write(struct framebuffer *fb, u64 addr, u8 *data, u32 len)
{
	memcpy(&fb->mem[addr - fb->mem_addr], data, len);
}

static void *vnc__thread(void *p)
{
	struct framebuffer *fb = p;
	/*
	 * Make a fake argc and argv because the getscreen function
	 * seems to want it.
	 */
	char argv[1][1] = {{0}};
	int argc = 1;

	rfbScreenInfoPtr server;

	server = rfbGetScreen(&argc, (char **) argv, fb->width, fb->height, 8, 3, 4);
	server->frameBuffer		= fb->mem;
	server->alwaysShared		= TRUE;
	server->kbdAddEvent		= kbd_handle_key;
	server->ptrAddEvent		= kbd_handle_ptr;
	rfbInitServer(server);

	while (rfbIsActive(server)) {
		rfbMarkRectAsModified(server, 0, 0, fb->width, fb->height);
		rfbProcessEvents(server, server->deferUpdateTime * VESA_UPDATE_TIME);
	}
	return NULL;
}

static int vnc__start(struct framebuffer *fb)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, vnc__thread, fb) != 0)
		return -1;

	return 0;
}

static struct fb_target_operations vnc_ops = {
	.start			= vnc__start,
	.write			= vnc__write,
};

void vnc__init(struct framebuffer *fb)
{
	fb__attach(fb, &vnc_ops);
}
