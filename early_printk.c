#include "kvm/early_printk.h"

#include "kvm/ioport.h"
#include "kvm/util.h"

#include <stdbool.h>

/* Transmitter holding register */
#define THR             0

/* Receive buffer register */
#define RBR             0

/* Divisor latch low byte */
#define DLL		0

/* Divisor latch high byte */
#define DLM		1

/* Interrupt enable register */
#define IER		1

/* Interrupt identification register */
#define IIR		2

/* 16550 FIFO Control Register */
#define FCR		2

/* Line control register */
#define LCR		3
enum {
	DLAB		= 1 << 7,		/* Divisor latch access bit (DLAB) */
	/* bit 7 - set break enable */
	PM2		= 1 << 5,
	PM1		= 1 << 4,
	PM0		= 1 << 3,
	STB		= 1 << 2,
	WLS1		= 1 << 1,
	WLS0		= 1 << 0,
};

/* Modem control register */
#define MCR		4

/* Line status register */
#define LSR		5

/* Modem status register */
#define MSR		6

/* Scratch register */
#define SCR		7

struct serial8250_device {
	uint16_t		iobase;
	uint8_t			dll;
	uint8_t			dlm;
	uint8_t			ier;
	uint8_t			fcr;
	uint8_t			lcr;
	uint8_t			mcr;
	uint8_t			scr;
};

static struct serial8250_device device = {
	.iobase			= 0x3f8,	/* ttyS0 */
};

static bool serial8250_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint16_t offset = port - device.iobase;

	if (device.lcr & DLAB) {
		switch (offset) {
		case DLL:
			device.dll		= ioport__read8(data);
			break;
		case DLM:
			device.dlm		= ioport__read8(data);
			break;
		case FCR:
			device.fcr		= ioport__read8(data);
			break;
		case LCR:
			device.lcr		= ioport__read8(data);
			break;
		default:
			return false;
		}
	} else {
		switch (offset) {
		case THR: {
			char *p = data;
			int i;

			while (count--) {
				for (i = 0; i < size; i++)
					fprintf(stdout, "%c", *p++);
			}
			fflush(stdout);
			break;
		}
		case IER:
			device.ier		= ioport__read8(data);
			break;
		case FCR:
			device.fcr		= ioport__read8(data);
			break;
		case LCR:
			device.lcr		= ioport__read8(data);
			break;
		case MCR:
			device.mcr		= ioport__read8(data);
			break;
		case SCR:
			device.scr		= ioport__read8(data);
			break;
		default:
			return false;
		}
	}

	return true;
}

static bool serial8250_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint16_t offset = port - device.iobase;

	if (device.lcr & DLAB)
		return false;

	switch (offset) {
	case THR:
		ioport__write8(data, 0x00);
		break;
	case IER:
		ioport__write8(data, device.ier);
		break;
	case IIR:
		ioport__write8(data, 0x01); /* no interrupt pending */
		break;
	case LCR:
		ioport__write8(data, device.lcr);
		break;
	case MCR:
		ioport__write8(data, device.mcr);
		break;
	case LSR:
		ioport__write8(data, 0x20); /* XMTRDY */
		break;
	case MSR:
		ioport__write8(data, 0x01); /* clear to send */
		break;
	case SCR:
		ioport__write8(data, device.scr);
		break;
	default:
		return false;
	}

	return true;
}

static struct ioport_operations serial8250_ops = {
	.io_in		= serial8250_in,
	.io_out		= serial8250_out,
};

void early_printk__init(void)
{
	ioport__register(device.iobase, &serial8250_ops, 8);
}
