#include "kvm/sdl.h"

#include "kvm/framebuffer.h"
#include "kvm/i8042.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/vesa.h"

#include <SDL/SDL.h>
#include <pthread.h>
#include <signal.h>
#include <linux/err.h>

#define FRAME_RATE		25

#define SCANCODE_UNKNOWN      0
#define SCANCODE_NORMAL       1
#define SCANCODE_ESCAPED      2
#define SCANCODE_KEY_PAUSE    3
#define SCANCODE_KEY_PRNTSCRN 4

struct set2_scancode {
	u8 code;
	u8 type;
};

#define DEFINE_SC(_code) {\
	.code = _code,\
	.type = SCANCODE_NORMAL,\
}

/* escaped scancodes */
#define DEFINE_ESC(_code) {\
	.code = _code,\
	.type = SCANCODE_ESCAPED,\
}

static const struct set2_scancode const keymap[256] = {
	[9]	= DEFINE_SC(0x76),	/* <esc> */
	[10]	= DEFINE_SC(0x16),	/* 1 */
	[11]	= DEFINE_SC(0x1e),	/* 2 */
	[12]	= DEFINE_SC(0x26),	/* 3 */
	[13]	= DEFINE_SC(0x25),	/* 4 */
	[14]	= DEFINE_SC(0x2e),	/* 5 */
	[15]	= DEFINE_SC(0x36),	/* 6 */
	[16]	= DEFINE_SC(0x3d),	/* 7 */
	[17]	= DEFINE_SC(0x3e),	/* 8 */
	[18]	= DEFINE_SC(0x46),	/* 9 */
	[19]	= DEFINE_SC(0x45),	/* 9 */
	[20]	= DEFINE_SC(0x4e),	/* - */
	[21]	= DEFINE_SC(0x55),	/* + */
	[22]	= DEFINE_SC(0x66),	/* <backspace> */
	[23]	= DEFINE_SC(0x0d),	/* <tab> */
	[24]	= DEFINE_SC(0x15),	/* q */
	[25]	= DEFINE_SC(0x1d),	/* w */
	[26]	= DEFINE_SC(0x24),	/* e */
	[27]	= DEFINE_SC(0x2d),	/* r */
	[28]	= DEFINE_SC(0x2c),	/* t */
	[29]	= DEFINE_SC(0x35),	/* y */
	[30]	= DEFINE_SC(0x3c),	/* u */
	[31]	= DEFINE_SC(0x43),	/* i */
	[32]	= DEFINE_SC(0x44),	/* o */
	[33]	= DEFINE_SC(0x4d),	/* p */
	[34]	= DEFINE_SC(0x54),	/* [ */
	[35]	= DEFINE_SC(0x5b),	/* ] */
	[36]	= DEFINE_SC(0x5a),	/* <enter> */
	[37]	= DEFINE_SC(0x14),	/* <left ctrl> */
	[38]	= DEFINE_SC(0x1c),	/* a */
	[39]	= DEFINE_SC(0x1b),	/* s */
	[40]	= DEFINE_SC(0x23),	/* d */
	[41]	= DEFINE_SC(0x2b),	/* f */
	[42]	= DEFINE_SC(0x34),	/* g */
	[43]	= DEFINE_SC(0x33),	/* h */
	[44]	= DEFINE_SC(0x3b),	/* j */
	[45]	= DEFINE_SC(0x42),	/* k */
	[46]	= DEFINE_SC(0x4b),	/* l */
	[47]	= DEFINE_SC(0x4c),	/* ; */
	[48]	= DEFINE_SC(0x52),	/* ' */
	[49]	= DEFINE_SC(0x0e),	/* ` */
	[50]	= DEFINE_SC(0x12),	/* <left shift> */
	[51]	= DEFINE_SC(0x5d),	/* \ */
	[52]	= DEFINE_SC(0x1a),	/* z */
	[53]	= DEFINE_SC(0x22),	/* x */
	[54]	= DEFINE_SC(0x21),	/* c */
	[55]	= DEFINE_SC(0x2a),	/* v */
	[56]	= DEFINE_SC(0x32),	/* b */
	[57]	= DEFINE_SC(0x31),	/* n */
	[58]	= DEFINE_SC(0x3a),	/* m */
	[59]	= DEFINE_SC(0x41),	/* < */
	[60]	= DEFINE_SC(0x49),	/* > */
	[61]	= DEFINE_SC(0x4a),	/* / */
	[62]	= DEFINE_SC(0x59),	/* <right shift> */
	[63]	= DEFINE_SC(0x7c),	/* keypad * */
	[64]	= DEFINE_SC(0x11),	/* <left alt> */
	[65]	= DEFINE_SC(0x29),	/* <space> */

	[67]	= DEFINE_SC(0x05),	/* <F1> */
	[68]	= DEFINE_SC(0x06),	/* <F2> */
	[69]	= DEFINE_SC(0x04),	/* <F3> */
	[70]	= DEFINE_SC(0x0c),	/* <F4> */
	[71]	= DEFINE_SC(0x03),	/* <F5> */
	[72]	= DEFINE_SC(0x0b),	/* <F6> */
	[73]	= DEFINE_SC(0x83),	/* <F7> */
	[74]	= DEFINE_SC(0x0a),	/* <F8> */
	[75]	= DEFINE_SC(0x01),	/* <F9> */
	[76]	= DEFINE_SC(0x09),	/* <F10> */

	[79]	= DEFINE_SC(0x6c),	/* keypad 7 */
	[80]	= DEFINE_SC(0x75),	/* keypad 8 */
	[81]	= DEFINE_SC(0x7d),	/* keypad 9 */
	[82]	= DEFINE_SC(0x7b),	/* keypad - */
	[83]	= DEFINE_SC(0x6b),	/* keypad 4 */
	[84]	= DEFINE_SC(0x73),	/* keypad 5 */
	[85]	= DEFINE_SC(0x74),	/* keypad 6 */
	[86]	= DEFINE_SC(0x79),	/* keypad + */
	[87]	= DEFINE_SC(0x69),	/* keypad 1 */
	[88]	= DEFINE_SC(0x72),	/* keypad 2 */
	[89]	= DEFINE_SC(0x7a),	/* keypad 3 */
	[90]	= DEFINE_SC(0x70),	/* keypad 0 */
	[91]	= DEFINE_SC(0x71),	/* keypad . */

	[94]	= DEFINE_SC(0x61),	/* <INT 1> */
	[95]	= DEFINE_SC(0x78),	/* <F11> */
	[96]	= DEFINE_SC(0x07),	/* <F12> */

	[104]	= DEFINE_ESC(0x5a),	/* keypad <enter> */
	[105]	= DEFINE_ESC(0x14),	/* <right ctrl> */
	[106]	= DEFINE_ESC(0x4a),	/* keypad / */
	[108]	= DEFINE_ESC(0x11),	/* <right alt> */
	[110]	= DEFINE_ESC(0x6c),	/* <home> */
	[111]	= DEFINE_ESC(0x75),	/* <up> */
	[112]	= DEFINE_ESC(0x7d),	/* <pag up> */
	[113]	= DEFINE_ESC(0x6b),	/* <left> */
	[114]	= DEFINE_ESC(0x74),	/* <right> */
	[115]	= DEFINE_ESC(0x69),	/* <end> */
	[116]	= DEFINE_ESC(0x72),	/* <down> */
	[117]	= DEFINE_ESC(0x7a),	/* <pag down> */
	[118]	= DEFINE_ESC(0x70),	/* <ins> */
	[119]	= DEFINE_ESC(0x71),	/* <delete> */
};
static bool running, done;

static const struct set2_scancode *to_code(u8 scancode)
{
	return &keymap[scancode];
}

static void key_press(const struct set2_scancode *sc)
{
	switch (sc->type) {
	case SCANCODE_ESCAPED:
		kbd_queue(0xe0);
		/* fallthrough */
	case SCANCODE_NORMAL:
		kbd_queue(sc->code);
		break;
	case SCANCODE_KEY_PAUSE:
		kbd_queue(0xe1);
		kbd_queue(0x14);
		kbd_queue(0x77);
		kbd_queue(0xe1);
		kbd_queue(0xf0);
		kbd_queue(0x14);
		kbd_queue(0x77);
		break;
	case SCANCODE_KEY_PRNTSCRN:
		kbd_queue(0xe0);
		kbd_queue(0x12);
		kbd_queue(0xe0);
		kbd_queue(0x7c);
		break;
	}
}

static void key_release(const struct set2_scancode *sc)
{
	switch (sc->type) {
	case SCANCODE_ESCAPED:
		kbd_queue(0xe0);
		/* fallthrough */
	case SCANCODE_NORMAL:
		kbd_queue(0xf0);
		kbd_queue(sc->code);
		break;
	case SCANCODE_KEY_PAUSE:
		/* nothing to do */
		break;
	case SCANCODE_KEY_PRNTSCRN:
		kbd_queue(0xe0);
		kbd_queue(0xf0);
		kbd_queue(0x7c);
		kbd_queue(0xe0);
		kbd_queue(0xf0);
		kbd_queue(0x12);
		break;
	}
}

static void *sdl__thread(void *p)
{
	Uint32 rmask, gmask, bmask, amask;
	struct framebuffer *fb = p;
	SDL_Surface *guest_screen;
	SDL_Surface *screen;
	SDL_Event ev;
	Uint32 flags;

	kvm__set_thread_name("kvm-sdl-worker");

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

	while (running) {
		SDL_BlitSurface(guest_screen, NULL, screen, NULL);
		SDL_Flip(screen);

		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_KEYDOWN: {
				const struct set2_scancode *sc = to_code(ev.key.keysym.scancode);
				if (sc->type == SCANCODE_UNKNOWN) {
					pr_warning("key '%d' not found in keymap", ev.key.keysym.scancode);
					break;
				}
				key_press(sc);
				break;
			}
			case SDL_KEYUP: {
				const struct set2_scancode *sc = to_code(ev.key.keysym.scancode);
				if (sc->type == SCANCODE_UNKNOWN)
					break;
				key_release(sc);
				break;
			}
			case SDL_QUIT:
				goto exit;
			}
		}

		SDL_Delay(1000 / FRAME_RATE);
	}

	if (running == false && done == false) {
		done = true;
		return NULL;
	}
exit:
	kvm_cpu__reboot(fb->kvm);

	return NULL;
}

static int sdl__start(struct framebuffer *fb)
{
	pthread_t thread;

	running = true;

	if (pthread_create(&thread, NULL, sdl__thread, fb) != 0)
		return -1;

	return 0;
}

static int sdl__stop(struct framebuffer *fb)
{
	running = false;
	while (done == false)
		sleep(0);

	return 0;
}

static struct fb_target_operations sdl_ops = {
	.start	= sdl__start,
	.stop	= sdl__stop,
};

int sdl__init(struct kvm *kvm)
{
	struct framebuffer *fb;

	if (!kvm->cfg.sdl)
		return 0;

	fb = vesa__init(kvm);
	if (IS_ERR(fb)) {
		pr_err("vesa__init() failed with error %ld\n", PTR_ERR(fb));
		return PTR_ERR(fb);
	}

	return fb__attach(fb, &sdl_ops);
}
dev_init(sdl__init);

int sdl__exit(struct kvm *kvm)
{
	if (kvm->cfg.sdl)
		return sdl__stop(NULL);

	return 0;
}
dev_exit(sdl__exit);
