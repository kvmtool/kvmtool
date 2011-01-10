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

	.lsr			= UART_LSR_TEMT | UART_LSR_THRE,
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
	uint8_t new_iir;

	device.iir	= UART_IIR_NO_INT;

	/* No interrupts enabled. Exit... */
	if (!(device.ier & (UART_IER_THRI|UART_IER_RDI)))
		return;

	new_iir		= 0;

	/* We're always good for guest sending data. */
	if (device.ier & UART_IER_THRI)
		new_iir			|= UART_IIR_THRI;

	/* Is there input in stdin to send to the guest? */
	if (!(device.lsr & UART_LSR_DR) && is_readable(fileno(stdin))) {
		int c;

		c			= read_char(fileno(stdin));
		if (c >= 0) {
			device.thr		= c;
			device.lsr		|= UART_LSR_DR;
			new_iir			|= UART_IIR_RDI;
		}
	}

	/* Only send an IRQ if there's work to do. */
	if (new_iir) {
		device.iir		= new_iir;
		kvm__irq_line(self, device.irq, 0);
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
