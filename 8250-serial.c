#include "kvm/8250-serial.h"

#include "kvm/read-write.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/kvm.h"

#include <linux/serial_reg.h>


struct serial8250_device {
	uint16_t		iobase;
	uint8_t			irq;

	uint8_t			rbr;		/* receive buffer */
	uint8_t			dll;
	uint8_t			dlm;
	uint8_t			iir;
	uint8_t			ier;
	uint8_t			fcr;
	uint8_t			lcr;
	uint8_t			mcr;
	uint8_t			lsr;
	uint8_t			msr;
	uint8_t			scr;
};

static struct serial8250_device devices[] = {
	/* ttyS0 */
	[0]	= {
		.iobase			= 0x3f8,
		.irq			= 4,

		.iir			= UART_IIR_NO_INT,
		.lsr			= UART_LSR_TEMT | UART_LSR_THRE,
		.msr			= UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS,
		.mcr			= UART_MCR_OUT2,
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

static void serial8250__receive(struct kvm *self, struct serial8250_device *dev)
{
	int c;

	if (dev->lsr & UART_LSR_DR)
		return;

	if (!term_readable(CONSOLE_8250))
		return;

	c		= term_getc(CONSOLE_8250);

	if (c < 0)
		return;

	dev->rbr	= c;
	dev->lsr	|= UART_LSR_DR;
}

/*
 * Interrupts are injected for ttyS0 only.
 */
void serial8250__inject_interrupt(struct kvm *self)
{
	struct serial8250_device *dev = &devices[0];

	serial8250__receive(self, dev);

	if (dev->ier & UART_IER_RDI && dev->lsr & UART_LSR_DR)
		dev->iir		= UART_IIR_RDI;
	else if (dev->ier & UART_IER_THRI)
		dev->iir		= UART_IIR_THRI;
	else
		dev->iir		= UART_IIR_NO_INT;

	if (dev->iir != UART_IIR_NO_INT) {
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
			dev->dll	= ioport__read8(data);
			break;
		case UART_DLM:
			dev->dlm	= ioport__read8(data);
			break;
		case UART_FCR:
			dev->fcr	= ioport__read8(data);
			break;
		case UART_LCR:
			dev->lcr	= ioport__read8(data);
			break;
		case UART_MCR:
			dev->mcr	= ioport__read8(data);
			break;
		case UART_LSR:
			/* Factory test */
			break;
		case UART_MSR:
			/* Not used */
			break;
		case UART_SCR:
			dev->scr	= ioport__read8(data);
			break;
		default:
			return false;
		}
	} else {
		switch (offset) {
		case UART_TX: {
			char *addr = data;
			if (!(dev->mcr & UART_MCR_LOOP)) {
				term_putc(CONSOLE_8250, addr, size * count);
			}
			dev->iir		= UART_IIR_NO_INT;
			break;
		}
		case UART_FCR:
			dev->fcr	= ioport__read8(data);
			break;
		case UART_IER:
			dev->ier	= ioport__read8(data) & 0x3f;
			break;
		case UART_LCR:
			dev->lcr	= ioport__read8(data);
			break;
		case UART_MCR:
			dev->mcr	= ioport__read8(data);
			break;
		case UART_LSR:
			/* Factory test */
			break;
		case UART_MSR:
			/* Not used */
			break;
		case UART_SCR:
			dev->scr	= ioport__read8(data);
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

	if (dev->lcr & UART_LCR_DLAB) {
		switch (offset) {
		case UART_DLL:
			ioport__write8(data, dev->dll);
			return true;
		case UART_DLM:
			ioport__write8(data, dev->dlm);
			return true;
		default:
			break;
		}
	} else {
		switch (offset) {
		case UART_RX:
			ioport__write8(data, dev->rbr);
			dev->lsr		&= ~UART_LSR_DR;
			dev->iir		= UART_IIR_NO_INT;
			return true;
		case UART_IER:
			ioport__write8(data, dev->ier);
			return true;
		default:
			break;
		}
	}

	switch (offset) {
	case UART_IIR: {
		uint8_t iir = dev->iir;

		if (dev->fcr & UART_FCR_ENABLE_FIFO)
			iir		|= 0xc0;

		ioport__write8(data, iir);
		break;
	}
	case UART_LCR:
		ioport__write8(data, dev->lcr);
		break;
	case UART_MCR:
		ioport__write8(data, dev->mcr);
		break;
	case UART_LSR:
		ioport__write8(data, dev->lsr);
		dev->lsr		&= ~(UART_LSR_OE|UART_LSR_PE|UART_LSR_FE|UART_LSR_BI);
		break;
	case UART_MSR:
		ioport__write8(data, dev->msr);
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

static void serial8250__device_init(struct kvm *kvm, struct serial8250_device *dev)
{
	ioport__register(dev->iobase, &serial8250_ops, 8);
	kvm__irq_line(kvm, dev->irq, 0);
}

void serial8250__init(struct kvm *kvm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		serial8250__device_init(kvm, dev);
	}
}
