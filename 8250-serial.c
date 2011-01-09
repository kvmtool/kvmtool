#include "kvm/8250-serial.h"

#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <stdbool.h>
#include <poll.h>

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

#define UART_IER_RDI		0x01
#define UART_IER_THRI		0x02

/* Interrupt identification register */
#define IIR		2

#define UART_IIR_NO_INT		0x01
#define UART_IIR_THRI		0x02

/* 16550 FIFO Control Register */
#define FCR		2

/* Line control register */
#define LCR		3

#define UART_LCR_DLAB		0x80

/* Modem control register */
#define MCR		4

/* Line status register */
#define LSR		5

#define UART_LSR_DR		0x01
#define UART_LSR_THRE		0x20

/* Modem status register */
#define MSR		6

#define UART_MSR_CTS		0x10

/* Scratch register */
#define SCR		7

struct serial8250_device {
	uint16_t		iobase;
	uint8_t			irq;

	uint8_t			thr;
	uint8_t			dll;
	uint8_t			dlm;
	uint8_t			iir;
	uint8_t			ier;
	uint8_t			fcr;
	uint8_t			lcr;
	uint8_t			mcr;
	uint8_t			lsr;
	uint8_t			scr;
};

static struct serial8250_device device = {
	.iobase			= 0x3f8,	/* ttyS0 */
	.irq			= 4,

	.iir			= UART_IIR_NO_INT,
	.lsr			= UART_LSR_THRE,
};

static int read_char(int fd)
{
	int c;

	if (read(fd, &c, 1) < 0)
		return -1;

	return c;
}

static bool is_readable(int fd)
{
	struct pollfd pollfd;
	int err;

	pollfd		= (struct pollfd) {
		.fd		= fd,
		.events		= POLLIN,
	};

	err		= poll(&pollfd, 1, 0);
	return err > 0;
}

void serial8250__interrupt(struct kvm *self)
{
	if (!(device.lsr & UART_LSR_DR) && is_readable(fileno(stdin))) {
		int c;

		c			= read_char(fileno(stdin));
		if (c >= 0) {
			device.thr		= c;
			device.lsr		|= UART_LSR_DR;
		}
	}

	if (device.ier & UART_IER_THRI || device.lsr & UART_LSR_DR) {
		device.iir		&= ~UART_IIR_NO_INT;
		kvm__irq_line(self, device.irq, 1);
	}
}

static bool serial8250_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint16_t offset = port - device.iobase;

	if (device.lcr & UART_LCR_DLAB) {
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

			device.iir		|= UART_IIR_NO_INT;
			kvm__irq_line(self, device.irq, 0);

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

	if (device.lcr & UART_LCR_DLAB)
		return false;

	switch (offset) {
	case THR:
		if (device.lsr & UART_LSR_DR) {
			device.lsr		&= ~UART_LSR_DR;
			ioport__write8(data, device.thr);

			device.iir		|= UART_IIR_NO_INT;
			kvm__irq_line(self, device.irq, 0);
		}
		break;
	case IER:
		ioport__write8(data, device.ier);
		break;
	case IIR:
		ioport__write8(data, device.iir);
		break;
	case LCR:
		ioport__write8(data, device.lcr);
		break;
	case MCR:
		ioport__write8(data, device.mcr);
		break;
	case LSR:
		ioport__write8(data, device.lsr);
		break;
	case MSR:
		ioport__write8(data, UART_MSR_CTS);
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

void serial8250__init(void)
{
	ioport__register(device.iobase, &serial8250_ops, 8);
}
