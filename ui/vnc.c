#include "kvm/vnc.h"

#include "kvm/framebuffer.h"
#include "kvm/i8042.h"
#include "kvm/vesa.h"

#include <linux/types.h>
#include <rfb/keysym.h>
#include <rfb/rfb.h>
#include <pthread.h>
#include <linux/err.h>

#define VESA_QUEUE_SIZE		128
#define VESA_IRQ		14

/*
 * This "6000" value is pretty much the result of experimentation
 * It seems that around this value, things update pretty smoothly
 */
#define VESA_UPDATE_TIME	6000

/*
 * We can map the letters and numbers without a fuss,
 * but the other characters not so much.
 */
static char letters[26] = {
	0x1c, 0x32, 0x21, 0x23, 0x24, /* a-e */
	0x2b, 0x34, 0x33, 0x43, 0x3b, /* f-j */
	0x42, 0x4b, 0x3a, 0x31, 0x44, /* k-o */
	0x4d, 0x15, 0x2d, 0x1b, 0x2c, /* p-t */
	0x3c, 0x2a, 0x1d, 0x22, 0x35, /* u-y */
	0x1a,
};

static rfbScreenInfoPtr server;
static char num[10] = {
	0x45, 0x16, 0x1e, 0x26, 0x2e, 0x23, 0x36, 0x3d, 0x3e, 0x46,
};

/*
 * This is called when the VNC server receives a key event
 * The reason this function is such a beast is that we have
 * to convert from ASCII characters (which is what VNC gets)
 * to PC keyboard scancodes, which is what Linux expects to
 * get from its keyboard. ASCII and the scancode set don't
 * really seem to mesh in any good way beyond some basics with
 * the letters and numbers.
 */
static void kbd_handle_key(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
	char tosend = 0;

	if (key >= 0x41 && key <= 0x5a)
		key += 0x20; /* convert to lowercase */

	if (key >= 0x61 && key <= 0x7a) /* a-z */
		tosend = letters[key - 0x61];

	if (key >= 0x30 && key <= 0x39)
		tosend = num[key - 0x30];

	switch (key) {
	case XK_Insert:		kbd_queue(0xe0);	tosend = 0x70;	break;
	case XK_Delete:		kbd_queue(0xe0);	tosend = 0x71;	break;
	case XK_Up:		kbd_queue(0xe0);	tosend = 0x75;	break;
	case XK_Down:		kbd_queue(0xe0);	tosend = 0x72;	break;
	case XK_Left:		kbd_queue(0xe0);	tosend = 0x6b;	break;
	case XK_Right:		kbd_queue(0xe0);	tosend = 0x74;	break;
	case XK_Page_Up:	kbd_queue(0xe0);	tosend = 0x7d;	break;
	case XK_Page_Down:	kbd_queue(0xe0);	tosend = 0x7a;	break;
	case XK_Home:		kbd_queue(0xe0);	tosend = 0x6c;	break;
	case XK_BackSpace:	tosend = 0x66;		break;
	case XK_Tab:		tosend = 0x0d;		break;
	case XK_Return:		tosend = 0x5a;		break;
	case XK_Escape:		tosend = 0x76;		break;
	case XK_End:		tosend = 0x69;		break;
	case XK_Shift_L:	tosend = 0x12;		break;
	case XK_Shift_R:	tosend = 0x59;		break;
	case XK_Control_R:	kbd_queue(0xe0);
	case XK_Control_L:	tosend = 0x14;		break;
	case XK_Alt_R:		kbd_queue(0xe0);
	case XK_Alt_L:		tosend = 0x11;		break;
	case XK_quoteleft:	tosend = 0x0e;		break;
	case XK_minus:		tosend = 0x4e;		break;
	case XK_equal:		tosend = 0x55;		break;
	case XK_bracketleft:	tosend = 0x54;		break;
	case XK_bracketright:	tosend = 0x5b;		break;
	case XK_backslash:	tosend = 0x5d;		break;
	case XK_Caps_Lock:	tosend = 0x58;		break;
	case XK_semicolon:	tosend = 0x4c;		break;
	case XK_quoteright:	tosend = 0x52;		break;
	case XK_comma:		tosend = 0x41;		break;
	case XK_period:		tosend = 0x49;		break;
	case XK_slash:		tosend = 0x4a;		break;
	case XK_space:		tosend = 0x29;		break;

	/*
	 * This is where I handle the shifted characters.
	 * They don't really map nicely the way A-Z maps to a-z,
	 * so I'm doing it manually
	 */
	case XK_exclam:		tosend = 0x16;		break;
	case XK_quotedbl:	tosend = 0x52;		break;
	case XK_numbersign:	tosend = 0x26;		break;
	case XK_dollar:		tosend = 0x25;		break;
	case XK_percent:	tosend = 0x2e;		break;
	case XK_ampersand:	tosend = 0x3d;		break;
	case XK_parenleft:	tosend = 0x46;		break;
	case XK_parenright:	tosend = 0x45;		break;
	case XK_asterisk:	tosend = 0x3e;		break;
	case XK_plus:		tosend = 0x55;		break;
	case XK_colon:		tosend = 0x4c;		break;
	case XK_less:		tosend = 0x41;		break;
	case XK_greater:	tosend = 0x49;		break;
	case XK_question:	tosend = 0x4a;		break;
	case XK_at:		tosend = 0x1e;		break;
	case XK_asciicircum:	tosend = 0x36;		break;
	case XK_underscore:	tosend = 0x4e;		break;
	case XK_braceleft:	tosend = 0x54;		break;
	case XK_braceright:	tosend = 0x5b;		break;
	case XK_bar:		tosend = 0x5d;		break;
	case XK_asciitilde:	tosend = 0x0e;		break;
	default:		break;
	}

	/*
	 * If this is a "key up" event (the user has released the key, we
	 * need to send 0xf0 first.
	 */
	if (!down && tosend != 0x0)
		kbd_queue(0xf0);

	if (tosend)
		kbd_queue(tosend);
}

/* The previous X and Y coordinates of the mouse */
static int xlast, ylast = -1;

/*
 * This function is called by the VNC server whenever a mouse event occurs.
 */
static void kbd_handle_ptr(int buttonMask, int x, int y, rfbClientPtr cl)
{
	int dx, dy;
	char b1 = 0x8;

	/* The VNC mask and the PS/2 button encoding are the same */
	b1 |= buttonMask;

	if (xlast >= 0 && ylast >= 0) {
		/* The PS/2 mouse sends deltas, not absolutes */
		dx = x - xlast;
		dy = ylast - y;

		/* Set overflow bits if needed */
		if (dy > 255)
			b1 |= 0x80;
		if (dx > 255)
			b1 |= 0x40;

		/* Set negative bits if needed */
		if (dy < 0)
			b1 |= 0x20;
		if (dx < 0)
			b1 |= 0x10;

		mouse_queue(b1);
		mouse_queue(dx);
		mouse_queue(dy);
	}

	xlast = x;
	ylast = y;
	rfbDefaultPtrAddEvent(buttonMask, x, y, cl);
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

	kvm__set_thread_name("kvm-vnc-worker");

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

static int vnc__stop(struct framebuffer *fb)
{
	rfbShutdownServer(server, TRUE);

	return 0;
}

static struct fb_target_operations vnc_ops = {
	.start	= vnc__start,
	.stop	= vnc__stop,
};

int vnc__init(struct kvm *kvm)
{
	struct framebuffer *fb;

	if (!kvm->cfg.vnc)
		return 0;

	fb = vesa__init(kvm);
	if (IS_ERR(fb)) {
		pr_err("vesa__init() failed with error %ld\n", PTR_ERR(fb));
		return PTR_ERR(fb);
	}

	return fb__attach(fb, &vnc_ops);
}
dev_init(vnc__init);

int vnc__exit(struct kvm *kvm)
{
	if (kvm->cfg.vnc)
		return vnc__stop(NULL);

	return 0;
}
dev_exit(vnc__exit);
