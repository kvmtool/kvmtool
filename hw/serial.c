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

	u16			iobase;
	u8			irq;

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

		.iobase			= 0x3f8,
		.irq			= 4,

		SERIAL_REGS_SETTING
	},
	/* ttyS1 */
	[1]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.iobase			= 0x2f8,
		.irq			= 3,

		SERIAL_REGS_SETTING
	},
	/* ttyS2 */
	[2]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.iobase			= 0x3e8,
		.irq			= 4,

		SERIAL_REGS_SETTING
	},
	/* ttyS3 */
	[3]	= {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,

		.iobase			= 0x2e8,
		.irq			= 3,

		SERIAL_REGS_SETTING
	},
};

#define SYSRQ_PENDING_NONE		0
#define SYSRQ_PENDING_BREAK		1
#define SYSRQ_PENDING_CMD		2

static int sysrq_pending;

static void serial8250__sysrq(struct kvm *kvm, struct serial8250_device *dev)
{
	switch (sysrq_pending) {
	case SYSRQ_PENDING_BREAK:
		dev->lsr	|= UART_LSR_DR | UART_LSR_BI;

		sysrq_pending	= SYSRQ_PENDING_CMD;
		break;
	case SYSRQ_PENDING_CMD:
		dev->rbr	= 'p';
		dev->lsr	|= UART_LSR_DR;

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
void serial8250__inject_interrupt(struct kvm *kvm)
{
	struct serial8250_device *dev = &devices[0];

	mutex_lock(&dev->mutex);

	serial8250__receive(kvm, dev);

	if (dev->ier & UART_IER_RDI && dev->lsr & UART_LSR_DR)
		dev->iir		= UART_IIR_RDI;
	else if (dev->ier & UART_IER_THRI)
		dev->iir		= UART_IIR_THRI;
	else
		dev->iir		= UART_IIR_NO_INT;

	if (dev->iir != UART_IIR_NO_INT) {
		kvm__irq_line(kvm, dev->irq, 0);
		kvm__irq_line(kvm, dev->irq, 1);
	}

	mutex_unlock(&dev->mutex);
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

	dev		= find_device(port);
	if (!dev)
		return false;

	mutex_lock(&dev->mutex);

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
			ret		= false;
			goto out_unlock;
		}
	} else {
		switch (offset) {
		case UART_TX: {
			char *addr = data;

			if (!(dev->mcr & UART_MCR_LOOP))
				term_putc(CONSOLE_8250, addr, size);

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
			ret		= false;
			goto out_unlock;
		}
	}

out_unlock:
	mutex_unlock(&dev->mutex);

	return ret;
}

static bool serial8250_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	struct serial8250_device *dev;
	u16 offset;
	bool ret = true;

	dev		= find_device(port);
	if (!dev)
		return false;

	mutex_lock(&dev->mutex);

	offset		= port - dev->iobase;

	if (dev->lcr & UART_LCR_DLAB) {
		switch (offset) {
		case UART_DLL:
			ioport__write8(data, dev->dll);
			goto out_unlock;

		case UART_DLM:
			ioport__write8(data, dev->dlm);
			goto out_unlock;

		default:
			break;
		}
	} else {
		switch (offset) {
		case UART_RX:
			ioport__write8(data, dev->rbr);
			dev->lsr		&= ~UART_LSR_DR;
			dev->iir		= UART_IIR_NO_INT;
			goto out_unlock;

		case UART_IER:
			ioport__write8(data, dev->ier);
			goto out_unlock;

		default:
			break;
		}
	}

	switch (offset) {
	case UART_IIR: {
		u8 iir = dev->iir;

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
		ret		= false;
		goto out_unlock;
	}
out_unlock:
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
