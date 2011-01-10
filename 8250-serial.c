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

	uint8_t			counter;
};

static struct serial8250_device devices[] = {
	/* ttyS0 */
	[0]	= {
		.iobase			= 0x3f8,
		.irq			= 4,

		.lsr			= UART_LSR_TEMT | UART_LSR_THRE,
	},
	/* ttyS1 */
	[1]	= {
		.iobase			= 0x2f8,
		.irq			= 3,

		.iir			= UART_IIR_NO_INT,
	},
	/* ttyS2 */
	[2]	= {
		.iobase			= 0x3e8,
		.irq			= 4,

		.iir			= UART_IIR_NO_INT,
	},
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

/*
 * Interrupts are injected for ttyS0 only.
 */
void serial8250__interrupt(struct kvm *self)
{
	struct serial8250_device *dev = &devices[0];
	uint8_t new_iir;

	dev->iir	= UART_IIR_NO_INT;

	/* No interrupts enabled. Exit... */
	if (!(dev->ier & (UART_IER_THRI|UART_IER_RDI)))
		return;

	new_iir		= 0;

	/* We're always good for guest sending data. */
	if (dev->ier & UART_IER_THRI)
		new_iir			|= UART_IIR_THRI;

	/* Is there input in stdin to send to the guest? */
	if (!(dev->lsr & UART_LSR_DR) && is_readable(fileno(stdin))) {
		int c;

		c			= read_char(fileno(stdin));
		if (c >= 0) {
			dev->thr		= c;
			dev->lsr		|= UART_LSR_DR;
			new_iir			|= UART_IIR_RDI;
		}
	}

	/* Only send an IRQ if there's work to do. */
	if (new_iir) {
		dev->counter		= 0;
		dev->iir		= new_iir;
		kvm__irq_line(self, dev->irq, 0);
		kvm__irq_line(self, dev->irq, 1);
	}
}

static struct serial8250_device *find_device(uint16_t port)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		if (dev->iobase == (port & ~0x7))
			return dev;
	}
	return NULL;
}

static bool serial8250_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	struct serial8250_device *dev;
	uint16_t offset;

	dev		= find_device(port);
	if (!dev)
		return false;

	offset		= port - dev->iobase;

	if (dev->lcr & UART_LCR_DLAB) {
		switch (offset) {
		case UART_DLL:
			dev->dll		= ioport__read8(data);
			break;
		case UART_DLM:
			dev->dlm		= ioport__read8(data);
			break;
		case UART_FCR:
			dev->fcr		= ioport__read8(data);
			break;
		case UART_LCR:
			dev->lcr		= ioport__read8(data);
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

			if (dev->counter++ > 10)
				dev->iir		= UART_IIR_NO_INT;

			break;
		}
		case UART_IER:
			dev->ier		= ioport__read8(data);
			break;
		case UART_FCR:
			dev->fcr		= ioport__read8(data);
			break;
		case UART_LCR:
			dev->lcr		= ioport__read8(data);
			break;
		case UART_MCR:
			dev->mcr		= ioport__read8(data);
			break;
		case UART_SCR:
			dev->scr		= ioport__read8(data);
			break;
		default:
			return false;
		}
	}

	return true;
}

static bool serial8250_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	struct serial8250_device *dev;
	uint16_t offset;

	dev		= find_device(port);
	if (!dev)
		return false;

	offset		= port - dev->iobase;

	if (dev->lcr & UART_LCR_DLAB)
		return false;

	switch (offset) {
	case UART_TX:
		if (dev->lsr & UART_LSR_DR) {
			dev->iir		= UART_IIR_NO_INT;

			dev->lsr		&= ~UART_LSR_DR;
			ioport__write8(data, dev->thr);
		}
		break;
	case UART_IER:
		ioport__write8(data, dev->ier);
		break;
	case UART_IIR:
		ioport__write8(data, dev->iir);
		break;
	case UART_LCR:
		ioport__write8(data, dev->lcr);
		break;
	case UART_MCR:
		ioport__write8(data, dev->mcr);
		break;
	case UART_LSR:
		ioport__write8(data, dev->lsr);
		break;
	case UART_MSR:
		ioport__write8(data, UART_MSR_CTS);
		break;
	case UART_SCR:
		ioport__write8(data, dev->scr);
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
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		ioport__register(dev->iobase, &serial8250_ops, 8);
	}
}
