#include "kvm/read-write.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/kvm.h"
#include "kvm/i8042.h"
#include "kvm/kvm-cpu.h"

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
#define I8042_CMD_SYSTEM_RESET	0xFE

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
void mouse_queue(u8 c)
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
void kbd_queue(u8 c)
{
	if (state.kcount >= QUEUE_SIZE)
		return;

	state.kq[state.kwrite++ % QUEUE_SIZE] = c;

	state.kcount++;
	kbd_update_irq();
}

static void kbd_write_command(struct kvm *kvm, u8 val)
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
	case I8042_CMD_SYSTEM_RESET:
		kvm_cpu__reboot(kvm);
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
 * Called when the OS has written to one of the keyboard's ports (0x60 or 0x64)
 */
static bool kbd_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	switch (port) {
	case I8042_COMMAND_REG: {
		u8 value = kbd_read_status();
		ioport__write8(data, value);
		break;
	}
	case I8042_DATA_REG: {
		u32 value = kbd_read_data();
		ioport__write32(data, value);
		break;
	}
	default:
		return false;
	}

	return true;
}

static bool kbd_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	switch (port) {
	case I8042_COMMAND_REG: {
		u8 value = ioport__read8(data);
		kbd_write_command(kvm, value);
		break;
	}
	case I8042_DATA_REG: {
		u32 value = ioport__read32(data);
		kbd_write_data(value);
		break;
	}
	default:
		return false;
	}

	return true;
}

static struct ioport_operations kbd_ops = {
	.io_in		= kbd_in,
	.io_out		= kbd_out,
};

int kbd__init(struct kvm *kvm)
{
#ifndef CONFIG_X86
	return 0;
#endif

	kbd_reset();
	state.kvm = kvm;
	ioport__register(kvm, I8042_DATA_REG, &kbd_ops, 2, NULL);
	ioport__register(kvm, I8042_COMMAND_REG, &kbd_ops, 2, NULL);

	return 0;
}
dev_init(kbd__init);
