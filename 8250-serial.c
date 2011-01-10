#include "kvm/8250-serial.h"

#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <linux/serial_reg.h>

#include <stdbool.h>
#include <poll.h>

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
	struct pollfd pollfd = (struct pollfd) {
		.fd	= fd,
		.events	= POLLIN,
	};

	return poll(&pollfd, 1, 0) > 0;
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
		case UART_DLL:
			device.dll		= ioport__read8(data);
			break;
		case UART_DLM:
			device.dlm		= ioport__read8(data);
			break;
		case UART_FCR:
			device.fcr		= ioport__read8(data);
			break;
		case UART_LCR:
			device.lcr		= ioport__read8(data);
			break;
		default:
			return false;
		}
	} else {
		switch (offset) {
		case UART_TX: {
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
		case UART_IER:
			device.ier		= ioport__read8(data);
			break;
		case UART_FCR:
			device.fcr		= ioport__read8(data);
			break;
		case UART_LCR:
			device.lcr		= ioport__read8(data);
			break;
		case UART_MCR:
			device.mcr		= ioport__read8(data);
			break;
		case UART_SCR:
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
	case UART_TX:
		if (device.lsr & UART_LSR_DR) {
			device.lsr		&= ~UART_LSR_DR;
			ioport__write8(data, device.thr);

			device.iir		|= UART_IIR_NO_INT;
			kvm__irq_line(self, device.irq, 0);
		}
		break;
	case UART_IER:
		ioport__write8(data, device.ier);
		break;
	case UART_IIR:
		ioport__write8(data, device.iir);
		break;
	case UART_LCR:
		ioport__write8(data, device.lcr);
		break;
	case UART_MCR:
		ioport__write8(data, device.mcr);
		break;
	case UART_LSR:
		ioport__write8(data, device.lsr);
		break;
	case UART_MSR:
		ioport__write8(data, UART_MSR_CTS);
		break;
	case UART_SCR:
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
