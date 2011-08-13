#include "kvm/sdl.h"

#include "kvm/framebuffer.h"
#include "kvm/i8042.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <SDL/SDL.h>
#include <pthread.h>
#include <signal.h>

#define FRAME_RATE		25

static u8 keymap[255] = {
	[10]		= 0x16,		/* 1 */
	[11]		= 0x1e,		/* 2 */
	[12]		= 0x26,		/* 3 */
	[13]		= 0x25,		/* 4 */
	[14]		= 0x27,		/* 5 */
	[15]		= 0x36,		/* 6 */
	[16]		= 0x3d,		/* 7 */
	[17]		= 0x3e,		/* 8 */
	[18]		= 0x46,		/* 9 */
	[19]		= 0x45,		/* 9 */
	[20]		= 0x4e,		/* - */
	[21]		= 0x55,		/* + */
	[22]		= 0x66,		/* <backspace> */

	[24]		= 0x15,		/* q */
	[25]		= 0x1d,		/* w */
	[26]		= 0x24,		/* e */
	[27]		= 0x2d,		/* r */
	[28]		= 0x2c,		/* t */
	[29]		= 0x35,		/* y */
	[30]		= 0x3c,		/* u */
	[31]		= 0x43,		/* i */
	[32]		= 0x44,		/* o */
	[33]		= 0x4d,		/* p */

	[36]		= 0x5a,		/* <enter> */

	[38]		= 0x1c,		/* a */
	[39]		= 0x1b,		/* s */
	[40]		= 0x23,		/* d */
	[41]		= 0x2b,		/* f */
	[42]		= 0x34,		/* g */
	[43]		= 0x33,		/* h */
	[44]		= 0x3b,		/* j */
	[45]		= 0x42,		/* k */
	[46]		= 0x4b,		/* l */

	[50]		= 0x12,		/* <left shift> */
	[51]		= 0x5d,		/* | */


	[52]		= 0x1a,		/* z */
	[53]		= 0x22,		/* x */
	[54]		= 0x21,		/* c */
	[55]		= 0x2a,		/* v */
	[56]		= 0x32,		/* b */
	[57]		= 0x31,		/* n */
	[58]		= 0x3a,		/* m */
	[59]		= 0x41,		/* < */
	[60]		= 0x49,		/* > */
	[61]		= 0x4a,		/* / */
	[62]		= 0x59,		/* <right shift> */
	[65]		= 0x29,		/* <space> */
};

static u8 to_code(u8 scancode)
{
	return keymap[scancode];
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

	flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL | SDL_DOUBLEBUF;

	SDL_WM_SetCaption("KVM tool", "KVM tool");

	screen = SDL_SetVideoMode(fb->width, fb->height, fb->depth, flags);
	if (!screen)
		die("Unable to set SDL video mode");

	SDL_EnableKeyRepeat(200, 50);

	for (;;) {
		SDL_BlitSurface(guest_screen, NULL, screen, NULL);
		SDL_Flip(screen);

		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_KEYDOWN: {
				u8 code = to_code(ev.key.keysym.scancode);
				if (code)
					kbd_queue(code);
				else
					pr_warning("key '%d' not found in keymap", ev.key.keysym.scancode);
				break;
			}
			case SDL_KEYUP: {
				u8 code = to_code(ev.key.keysym.scancode);
				if (code) {
					kbd_queue(0xf0);
					kbd_queue(code);
				}
				break;
			}
			case SDL_QUIT:
				goto exit;
			}
		}

		SDL_Delay(1000 / FRAME_RATE);
	}
exit:
	kill(0, SIGKVMSTOP);

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
};

void sdl__init(struct framebuffer *fb)
{
	fb__attach(fb, &sdl_ops);
}
