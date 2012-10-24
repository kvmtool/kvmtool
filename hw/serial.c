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

/*
 * This fakes a U6_16550A. The fifo len needs to be 64 as the kernel
 * expects that for autodetection.
 */
#define FIFO_LEN		64
#define FIFO_MASK		(FIFO_LEN - 1)

#define UART_IIR_TYPE_BITS	0xc0

struct serial8250_device {
	struct mutex		mutex;
	u8			id;

	u16			iobase;
	u8			irq;
	u8			irq_state;
	int			txcnt;
	int			rxcnt;
	int			rxdone;
	char			txbuf[FIFO_LEN];
	char			rxbuf[FIFO_LEN];

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
		.mutex			= MUTEX_INITIALIZER,

		.id			= 0,
		.iobase			= 0x3f8,
		.irq			= 4,

		SERIAL_REGS_SETTING
	},
	/* ttyS1 */
	[1]	= {
		.mutex			= MUTEX_INITIALIZER,

		.id			= 1,
		.iobase			= 0x2f8,
		.irq			= 3,

		SERIAL_REGS_SETTING
	},
	/* ttyS2 */
	[2]	= {
		.mutex			= MUTEX_INITIALIZER,

		.id			= 2,
		.iobase			= 0x3e8,
		.irq			= 4,

		SERIAL_REGS_SETTING
	},
	/* ttyS3 */
	[3]	= {
		.mutex			= MUTEX_INITIALIZER,

		.id			= 3,
		.iobase			= 0x2e8,
		.irq			= 3,

		SERIAL_REGS_SETTING
	},
};

static void serial8250_flush_tx(struct kvm *kvm, struct serial8250_device *dev)
{
	dev->lsr |= UART_LSR_TEMT | UART_LSR_THRE;

	if (dev->txcnt) {
		if (kvm->cfg.active_console == CONSOLE_8250)
			term_putc(dev->txbuf, dev->txcnt, dev->id);
		dev->txcnt = 0;
	}
}

static void serial8250_update_irq(struct kvm *kvm, struct serial8250_device *dev)
{
	u8 iir = 0;

	/* Handle clear rx */
	if (dev->lcr & UART_FCR_CLEAR_RCVR) {
		dev->lcr &= ~UART_FCR_CLEAR_RCVR;
		dev->rxcnt = dev->rxdone = 0;
		dev->lsr &= ~UART_LSR_DR;
	}

	/* Handle clear tx */
	if (dev->lcr & UART_FCR_CLEAR_XMIT) {
		dev->lcr &= ~UART_FCR_CLEAR_XMIT;
		dev->txcnt = 0;
		dev->lsr |= UART_LSR_TEMT | UART_LSR_THRE;
	}

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

	/*
	 * If the kernel disabled the tx interrupt, we know that there
	 * is nothing more to transmit, so we can reset our tx logic
	 * here.
	 */
	if (!(dev->ier & UART_IER_THRI))
		serial8250_flush_tx(kvm, dev);
}

#define SYSRQ_PENDING_NONE		0

static int sysrq_pending;

static void serial8250__sysrq(struct kvm *kvm, struct serial8250_device *dev)
{
	dev->lsr |= UART_LSR_DR | UART_LSR_BI;
	dev->rxbuf[dev->rxcnt++] = sysrq_pending;
	sysrq_pending	= SYSRQ_PENDING_NONE;
}

static void serial8250__receive(struct kvm *kvm, struct serial8250_device *dev,
				bool handle_sysrq)
{
	int c;

	/*
	 * If the guest transmitted a full fifo, we clear the
	 * TEMT/THRE bits to let the kernel escape from the 8250
	 * interrupt handler. We come here only once a ms, so that
	 * should give the kernel the desired pause. That also flushes
	 * the tx fifo to the terminal.
	 */
	serial8250_flush_tx(kvm, dev);

	if (dev->mcr & UART_MCR_LOOP)
		return;

	if ((dev->lsr & UART_LSR_DR) || dev->rxcnt)
		return;

	if (handle_sysrq && sysrq_pending) {
		serial8250__sysrq(kvm, dev);
		return;
	}

	if (kvm->cfg.active_console != CONSOLE_8250)
		return;

	while (term_readable(dev->id) &&
	       dev->rxcnt < FIFO_LEN) {

		c = term_getc(kvm, dev->id);

		if (c < 0)
			break;
		dev->rxbuf[dev->rxcnt++] = c;
		dev->lsr |= UART_LSR_DR;
	}
}

void serial8250__update_consoles(struct kvm *kvm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		mutex_lock(&dev->mutex);

		/* Restrict sysrq injection to the first port */
		serial8250__receive(kvm, dev, i == 0);

		serial8250_update_irq(kvm, dev);

		mutex_unlock(&dev->mutex);
	}
}

void serial8250__inject_sysrq(struct kvm *kvm, char sysrq)
{
	sysrq_pending = sysrq;
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

static bool serial8250_out(struct ioport *ioport, struct kvm *kvm, u16 port,
			   void *data, int size)
{
	struct serial8250_device *dev;
	u16 offset;
	bool ret = true;
	char *addr = data;

	dev = find_device(port);
	if (!dev)
		return false;

	mutex_lock(&dev->mutex);

	offset = port - dev->iobase;

	switch (offset) {
	case UART_TX:
		if (dev->lcr & UART_LCR_DLAB) {
			dev->dll = ioport__read8(data);
			break;
		}

		/* Loopback mode */
		if (dev->mcr & UART_MCR_LOOP) {
			if (dev->rxcnt < FIFO_LEN) {
				dev->rxbuf[dev->rxcnt++] = *addr;
				dev->lsr |= UART_LSR_DR;
			}
			break;
		}

		if (dev->txcnt < FIFO_LEN) {
			dev->txbuf[dev->txcnt++] = *addr;
			dev->lsr &= ~UART_LSR_TEMT;
			if (dev->txcnt == FIFO_LEN / 2)
				dev->lsr &= ~UART_LSR_THRE;
		} else {
			/* Should never happpen */
			dev->lsr &= ~(UART_LSR_TEMT | UART_LSR_THRE);
		}
		break;
	case UART_IER:
		if (!(dev->lcr & UART_LCR_DLAB))
			dev->ier = ioport__read8(data) & 0x0f;
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

static void serial8250_rx(struct serial8250_device *dev, void *data)
{
	if (dev->rxdone == dev->rxcnt)
		return;

	/* Break issued ? */
	if (dev->lsr & UART_LSR_BI) {
		dev->lsr &= ~UART_LSR_BI;
		ioport__write8(data, 0);
		return;
	}

	ioport__write8(data, dev->rxbuf[dev->rxdone++]);
	if (dev->rxcnt == dev->rxdone) {
		dev->lsr &= ~UART_LSR_DR;
		dev->rxcnt = dev->rxdone = 0;
	}
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
		if (dev->lcr & UART_LCR_DLAB)
			ioport__write8(data, dev->dll);
		else
			serial8250_rx(dev, data);
		break;
	case UART_IER:
		if (dev->lcr & UART_LCR_DLAB)
			ioport__write8(data, dev->dlm);
		else
			ioport__write8(data, dev->ier);
		break;
	case UART_IIR:
		ioport__write8(data, dev->iir | UART_IIR_TYPE_BITS);
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

static int serial8250__device_init(struct kvm *kvm, struct serial8250_device *dev)
{
	int r;

	r = ioport__register(kvm, dev->iobase, &serial8250_ops, 8, NULL);
	kvm__irq_line(kvm, dev->irq, 0);

	return r;
}

int serial8250__init(struct kvm *kvm)
{
	unsigned int i, j;
	int r = 0;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		r = serial8250__device_init(kvm, dev);
		if (r < 0)
			goto cleanup;
	}

	return r;
cleanup:
	for (j = 0; j <= i; j++) {
		struct serial8250_device *dev = &devices[j];

		ioport__unregister(kvm, dev->iobase);
	}

	return r;
}
dev_init(serial8250__init);

int serial8250__exit(struct kvm *kvm)
{
	unsigned int i;
	int r;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		r = ioport__unregister(kvm, dev->iobase);
		if (r < 0)
			return r;
	}

	return 0;
}
dev_exit(serial8250__exit);
