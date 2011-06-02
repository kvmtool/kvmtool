#include "kvm/read-write.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/kvm.h"
#include "kvm/i8042.h"

#include <rfb/keysym.h>
#include <rfb/rfb.h>
#include <stdint.h>

/*
 * IRQs
 */
#define KBD_IRQ			1
#define AUX_IRQ			12

/*
 * Registers
 */
#define I8042_DATA_REG		0x60
#define I8042_COMMAND_REG	0x64

/*
 * Commands
 */
#define I8042_CMD_CTL_RCTR	0x20
#define I8042_CMD_CTL_WCTR	0x60
#define I8042_CMD_AUX_LOOP	0xD3
#define I8042_CMD_AUX_SEND	0xD4
#define I8042_CMD_AUX_TEST	0xA9
#define I8042_CMD_AUX_DISABLE	0xA7
#define I8042_CMD_AUX_ENABLE	0xA8

#define RESPONSE_ACK		0xFA

#define MODE_DISABLE_AUX	0x20

#define AUX_ENABLE_REPORTING	0x20
#define AUX_SCALING_FLAG	0x10
#define AUX_DEFAULT_RESOLUTION	0x2
#define AUX_DEFAULT_SAMPLE	100

/*
 * Status register bits
 */
#define I8042_STR_AUXDATA	0x20
#define I8042_STR_KEYLOCK	0x10
#define I8042_STR_CMDDAT	0x08
#define I8042_STR_MUXERR	0x04
#define I8042_STR_OBF		0x01

#define KBD_MODE_KBD_INT	0x01
#define KBD_MODE_SYS		0x02

#define QUEUE_SIZE		128

/*
 * This represents the current state of the PS/2 keyboard system,
 * including the AUX device (the mouse)
 */
struct kbd_state {
	struct kvm		*kvm;

	char			kq[QUEUE_SIZE];	/* Keyboard queue */
	int			kread, kwrite;	/* Indexes into the queue */
	int			kcount;		/* number of elements in queue */

	char			mq[QUEUE_SIZE];
	int			mread, mwrite;
	int			mcount;

	u8			mstatus;	/* Mouse status byte */
	u8			mres;		/* Current mouse resolution */
	u8			msample;	/* Current mouse samples/second */

	u8			mode;		/* i8042 mode register */
	u8			status;		/* i8042 status register */
	/*
	 * Some commands (on port 0x64) have arguments;
	 * we store the command here while we wait for the argument
	 */
	u32			write_cmd;
};

static struct kbd_state		state;

/*
 * If there are packets to be read, set the appropriate IRQs high
 */
static void kbd_update_irq(void)
{
	u8 klevel = 0;
	u8 mlevel = 0;

	/* First, clear the kbd and aux output buffer full bits */
	state.status &= ~(I8042_STR_OBF | I8042_STR_AUXDATA);

	if (state.kcount > 0) {
		state.status |= I8042_STR_OBF;
		klevel = 1;
	}

	/* Keyboard has higher priority than mouse */
	if (klevel == 0 && state.mcount != 0) {
		state.status |= I8042_STR_OBF | I8042_STR_AUXDATA;
		mlevel = 1;
	}

	kvm__irq_line(state.kvm, KBD_IRQ, klevel);
	kvm__irq_line(state.kvm, AUX_IRQ, mlevel);
}

/*
 * Add a byte to the mouse queue, then set IRQs
 */
static void mouse_queue(u8 c)
{
	if (state.mcount >= QUEUE_SIZE)
		return;

	state.mq[state.mwrite++ % QUEUE_SIZE] = c;

	state.mcount++;
	kbd_update_irq();
}

/*
 * Add a byte to the keyboard queue, then set IRQs
 */
static void kbd_queue(u8 c)
{
	if (state.kcount >= QUEUE_SIZE)
		return;

	state.kq[state.kwrite++ % QUEUE_SIZE] = c;

	state.kcount++;
	kbd_update_irq();
}

/*
 * This function is called when the OS issues a write to port 0x64
 */
static void kbd_write_command(u32 val)
{
	switch (val) {
	case I8042_CMD_CTL_RCTR:
		kbd_queue(state.mode);
		break;
	case I8042_CMD_CTL_WCTR:
	case I8042_CMD_AUX_SEND:
	case I8042_CMD_AUX_LOOP:
		state.write_cmd = val;
		break;
	case I8042_CMD_AUX_TEST:
		/* 0 means we're a normal PS/2 mouse */
		mouse_queue(0);
		break;
	case I8042_CMD_AUX_DISABLE:
		state.mode |= MODE_DISABLE_AUX;
		break;
	case I8042_CMD_AUX_ENABLE:
		state.mode &= ~MODE_DISABLE_AUX;
		break;
	default:
		break;
	}
}

/*
 * Called when the OS reads from port 0x60 (PS/2 data)
 */
static u32 kbd_read_data(void)
{
	u32 ret;
	int i;

	if (state.kcount != 0) {
		/* Keyboard data gets read first */
		ret = state.kq[state.kread++ % QUEUE_SIZE];
		state.kcount--;
		kvm__irq_line(state.kvm, KBD_IRQ, 0);
		kbd_update_irq();
	} else if (state.mcount > 0) {
		/* Followed by the mouse */
		ret = state.mq[state.mread++ % QUEUE_SIZE];
		state.mcount--;
		kvm__irq_line(state.kvm, AUX_IRQ, 0);
		kbd_update_irq();
	} else if (state.kcount == 0) {
		i = state.kread - 1;
		if (i < 0)
			i = QUEUE_SIZE;
		ret = state.kq[i];
	}
	return ret;
}

/*
 * Called when the OS read from port 0x64, the command port
 */
static u32 kbd_read_status(void)
{
	return (u32)state.status;
}

/*
 * Called when the OS writes to port 0x60 (data port)
 * Things written here are generally arguments to commands previously
 * written to port 0x64 and stored in state.write_cmd
 */
static void kbd_write_data(u32 val)
{
	switch (state.write_cmd) {
	case I8042_CMD_CTL_WCTR:
		state.mode = val;
		kbd_update_irq();
		break;
	case I8042_CMD_AUX_LOOP:
		mouse_queue(val);
		mouse_queue(RESPONSE_ACK);
		break;
	case I8042_CMD_AUX_SEND:
		/* The OS wants to send a command to the mouse */
		mouse_queue(RESPONSE_ACK);
		switch (val) {
		case 0xe6:
			/* set scaling = 1:1 */
			state.mstatus &= ~AUX_SCALING_FLAG;
			break;
		case 0xe8:
			/* set resolution */
			state.mres = val;
			break;
		case 0xe9:
			/* Report mouse status/config */
			mouse_queue(state.mstatus);
			mouse_queue(state.mres);
			mouse_queue(state.msample);
			break;
		case 0xf2:
			/* send ID */
			mouse_queue(0); /* normal mouse */
			break;
		case 0xf3:
			/* set sample rate */
			state.msample = val;
			break;
		case 0xf4:
			/* enable reporting */
			state.mstatus |= AUX_ENABLE_REPORTING;
			break;
		case 0xf5:
			state.mstatus &= ~AUX_ENABLE_REPORTING;
			break;
		case 0xf6:
			/* set defaults, just fall through to reset */
		case 0xff:
			/* reset */
			state.mstatus = 0x0;
			state.mres = AUX_DEFAULT_RESOLUTION;
			state.msample = AUX_DEFAULT_SAMPLE;
			break;
		default:
			break;
	}
	break;
	case 0:
		/* Just send the ID */
		kbd_queue(RESPONSE_ACK);
		kbd_queue(0xab);
		kbd_queue(0x41);
		kbd_update_irq();
		break;
	default:
		/* Yeah whatever */
		break;
	}
	state.write_cmd = 0;
}

static void kbd_reset(void)
{
	state = (struct kbd_state) {
		.status		= I8042_STR_MUXERR | I8042_STR_CMDDAT | I8042_STR_KEYLOCK, /* 0x1c */
		.mode		= KBD_MODE_KBD_INT | KBD_MODE_SYS, /* 0x3 */
		.mres		= AUX_DEFAULT_RESOLUTION,
		.msample	= AUX_DEFAULT_SAMPLE,
	};
}

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
void kbd_handle_key(rfbBool down, rfbKeySym key, rfbClientPtr cl)
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
void kbd_handle_ptr(int buttonMask, int x, int y, rfbClientPtr cl)
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

/*
 * Called when the OS has written to one of the keyboard's ports (0x60 or 0x64)
 */
static bool kbd_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	u32 result;

	if (port == I8042_COMMAND_REG) {
		result = kbd_read_status();
		ioport__write8(data, (char)result);
	} else {
		result = kbd_read_data();
		ioport__write32(data, result);
	}
	return true;
}

/*
 * Called when the OS attempts to read from a keyboard port (0x60 or 0x64)
 */
static bool kbd_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	if (port == I8042_COMMAND_REG)
		kbd_write_command(*((u32 *)data));
	else
		kbd_write_data(*((u32 *)data));

	return true;
}

static struct ioport_operations kbd_ops = {
	.io_in		= kbd_in,
	.io_out		= kbd_out,
};

void kbd__init(struct kvm *kvm)
{
	kbd_reset();
	state.kvm = kvm;
	ioport__register(I8042_DATA_REG, &kbd_ops, 2, NULL);
	ioport__register(I8042_COMMAND_REG, &kbd_ops, 2, NULL);
}
