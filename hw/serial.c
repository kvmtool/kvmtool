#include "kvm/8250-serial.h"

#include "kvm/read-write.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/kvm.h"

#include <linux/types.h>
#include <linux/serial_reg.h>

#include <pthread.h>

struct serial8250_device {
	pthread_mutex_t		mutex;
	u8			id;

	u16			iobase;
	u8			irq;
	u8			irq_state;

	u8			rbr;		/* receive buffer */
	u8			dll;
	u8			dlm;
	u8			iir;
	u8			ier;
	u8			fcr;
	u8			lcr;
	u8			mcr;
	u8			lsr;
	u8			msr;
	u8			scr;
};

#define SERIAL_REGS_SETTING \
	.iir			= UART_IIR_NO_INT, \
	.lsr			= UART_LSR_TEMT | UART_LSR_THRE, \
	.msr			= UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS, \
	.mcr			= UART_MCR_OUT2,

static struct serial8250_device devices[] = {
	/* ttyS0 */
	[0]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.id			= 0,
		.iobase			= 0x3f8,
		.irq			= 4,

		SERIAL_REGS_SETTING
	},
	/* ttyS1 */
	[1]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.id			= 1,
		.iobase			= 0x2f8,
		.irq			= 3,

		SERIAL_REGS_SETTING
	},
	/* ttyS2 */
	[2]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.id			= 2,
		.iobase			= 0x3e8,
		.irq			= 4,

		SERIAL_REGS_SETTING
	},
	/* ttyS3 */
	[3]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.id			= 3,
		.iobase			= 0x2e8,
		.irq			= 3,

		SERIAL_REGS_SETTING
	},
};

static void serial8250_update_irq(struct kvm *kvm, struct serial8250_device *dev)
{
	u8 iir = 0;

	/* Data ready and rcv interrupt enabled ? */
	if ((dev->ier & UART_IER_RDI) && (dev->lsr & UART_LSR_DR))
		iir |= UART_IIR_RDI;

	/* Transmitter empty and interrupt enabled ? */
	if ((dev->ier & UART_IER_THRI) && (dev->lsr & UART_LSR_TEMT))
		iir |= UART_IIR_THRI;

	/* Now update the irq line, if necessary */
	if (!iir) {
		dev->iir = UART_IIR_NO_INT;
		if (dev->irq_state)
			kvm__irq_line(kvm, dev->irq, 0);
	} else {
		dev->iir = iir;
		if (!dev->irq_state)
			kvm__irq_line(kvm, dev->irq, 1);
	}
	dev->irq_state = iir;
}

#define SYSRQ_PENDING_NONE		0
#define SYSRQ_PENDING_BREAK		1
#define SYSRQ_PENDING_CMD		2

static int sysrq_pending;

static void serial8250__sysrq(struct kvm *kvm, struct serial8250_device *dev)
{
	switch (sysrq_pending) {
	case SYSRQ_PENDING_BREAK:
		dev->lsr |= UART_LSR_DR | UART_LSR_BI;

		sysrq_pending = SYSRQ_PENDING_CMD;
		break;
	case SYSRQ_PENDING_CMD:
		dev->rbr = 'p';
		dev->lsr |= UART_LSR_DR;

		sysrq_pending	= SYSRQ_PENDING_NONE;
		break;
	}
}

static void serial8250__receive(struct kvm *kvm, struct serial8250_device *dev)
{
	int c;

	if (dev->lsr & UART_LSR_DR)
		return;

	if (sysrq_pending) {
		serial8250__sysrq(kvm, dev);
		return;
	}

	if (!term_readable(CONSOLE_8250, dev->id))
		return;

	c = term_getc(CONSOLE_8250, dev->id);

	if (c < 0)
		return;

	dev->rbr = c;
	dev->lsr |= UART_LSR_DR;
}

void serial8250__update_consoles(struct kvm *kvm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		mutex_lock(&dev->mutex);

		serial8250__receive(kvm, dev);

		serial8250_update_irq(kvm, dev);

		mutex_unlock(&dev->mutex);
	}
}

void serial8250__inject_sysrq(struct kvm *kvm)
{
	sysrq_pending	= SYSRQ_PENDING_BREAK;
}

static struct serial8250_device *find_device(u16 port)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		if (dev->iobase == (port & ~0x7))
			return dev;
	}
	return NULL;
}

static bool serial8250_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	struct serial8250_device *dev;
	u16 offset;
	bool ret = true;

	dev = find_device(port);
	if (!dev)
		return false;

	mutex_lock(&dev->mutex);

	offset = port - dev->iobase;

	switch (offset) {
	case UART_TX:
		if (!(dev->lcr & UART_LCR_DLAB)) {
			char *addr = data;

			if (!(dev->mcr & UART_MCR_LOOP))
				term_putc(CONSOLE_8250, addr, size, dev->id);
			/* else FIXME: Inject data into rcv path for LOOP */

			/*
			 * Set transmitter and transmit hold register
			 * empty.  We have no FIFO at the moment and
			 * on the TX side it's only interesting, when
			 * we could coalesce port io on the kernel
			 * kernel.
			 */
			dev->lsr |= UART_LSR_TEMT | UART_LSR_THRE;
			break;
		} else {
			dev->dll = ioport__read8(data);
		}
		break;
	case UART_IER:
		if (!(dev->lcr & UART_LCR_DLAB))
			dev->ier = ioport__read8(data) & 0x3f;
		else
			dev->dlm = ioport__read8(data);
		break;
	case UART_FCR:
		dev->fcr = ioport__read8(data);
		break;
	case UART_LCR:
		dev->lcr = ioport__read8(data);
		break;
	case UART_MCR:
		dev->mcr = ioport__read8(data);
		break;
	case UART_LSR:
		/* Factory test */
		break;
	case UART_MSR:
		/* Not used */
		break;
	case UART_SCR:
		dev->scr = ioport__read8(data);
		break;
	default:
		ret = false;
		break;
	}

	serial8250_update_irq(kvm, dev);

	mutex_unlock(&dev->mutex);

	return ret;
}

static bool serial8250_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	struct serial8250_device *dev;
	u16 offset;
	bool ret = true;

	dev = find_device(port);
	if (!dev)
		return false;

	mutex_lock(&dev->mutex);

	offset = port - dev->iobase;

	switch (offset) {
	case UART_RX:
		if (dev->lcr & UART_LCR_DLAB) {
			ioport__write8(data, dev->dll);
		} else {
			ioport__write8(data, dev->rbr);
			dev->lsr &= ~UART_LSR_DR;
		}
		break;
	case UART_IER:
		if (dev->lcr & UART_LCR_DLAB)
			ioport__write8(data, dev->dlm);
		else
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
		ioport__write8(data, dev->msr);
		break;
	case UART_SCR:
		ioport__write8(data, dev->scr);
		break;
	default:
		ret = false;
		break;
	}

	serial8250_update_irq(kvm, dev);

	mutex_unlock(&dev->mutex);

	return ret;
}

static struct ioport_operations serial8250_ops = {
	.io_in		= serial8250_in,
	.io_out		= serial8250_out,
};

static void serial8250__device_init(struct kvm *kvm, struct serial8250_device *dev)
{
	ioport__register(dev->iobase, &serial8250_ops, 8, NULL);
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
