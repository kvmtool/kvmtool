#include "kvm/sdl.h"

#include "kvm/framebuffer.h"
#include "kvm/util.h"

#include <SDL/SDL.h>
#include <pthread.h>

#define FRAME_RATE		25

static void sdl__write(struct framebuffer *fb, u64 addr, u8 *data, u32 len)
{
	memcpy(&fb->mem[addr - fb->mem_addr], data, len);
}

static void *sdl__thread(void *p)
{
	Uint32 rmask, gmask, bmask, amask;
	struct framebuffer *fb = p;
	SDL_Surface *guest_screen;
	SDL_Surface *screen;
	SDL_Event ev;
	Uint32 flags;

	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		die("Unable to initialize SDL");

	rmask = 0x000000ff;
	gmask = 0x0000ff00;
	bmask = 0x00ff0000;
	amask = 0x00000000;

	guest_screen = SDL_CreateRGBSurfaceFrom(fb->mem, fb->width, fb->height, fb->depth, fb->width * fb->depth / 8, rmask, gmask, bmask, amask);
	if (!guest_screen)
		die("Unable to create SDL RBG surface");

	flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL;

	screen = SDL_SetVideoMode(fb->width, fb->height, fb->depth, flags);
	if (!screen)
		die("Unable to set SDL video mode");

	for (;;) {
		SDL_BlitSurface(guest_screen, NULL, screen, NULL);
		SDL_UpdateRect(screen, 0, 0, 0, 0);
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT:
				goto exit;
			}
		}
		SDL_Delay(1000 / FRAME_RATE);
	}
exit:
	return NULL;
}

static int sdl__start(struct framebuffer *fb)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, sdl__thread, fb) != 0)
		return -1;

	return 0;
}

static struct fb_target_operations sdl_ops = {
	.start			= sdl__start,
	.write			= sdl__write,
};

void sdl__init(struct framebuffer *fb)
{
	fb__attach(fb, &sdl_ops);
}
