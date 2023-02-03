#include "kvm/8250-serial.h"

#include "kvm/read-write.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/term.h"
#include "kvm/kvm.h"
#include "kvm/fdt.h"

#include <linux/types.h>
#include <linux/serial_reg.h>

#include <pthread.h>

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
#define serial_iobase(nr)	(ARM_UART_MMIO_BASE + (nr) * 0x1000)
#define serial_irq(nr)		(32 + (nr))
#define SERIAL8250_BUS_TYPE	DEVICE_BUS_MMIO
#elif defined(CONFIG_RISCV)
#define serial_iobase(nr)	(RISCV_UART_MMIO_BASE + (nr) * 0x1000)
#define serial_irq(nr)		(1 + (nr))
#define SERIAL8250_BUS_TYPE	DEVICE_BUS_MMIO
#else
#define serial_iobase_0		(KVM_IOPORT_AREA + 0x3f8)
#define serial_iobase_1		(KVM_IOPORT_AREA + 0x2f8)
#define serial_iobase_2		(KVM_IOPORT_AREA + 0x3e8)
#define serial_iobase_3		(KVM_IOPORT_AREA + 0x2e8)
#define serial_irq_0		4
#define serial_irq_1		3
#define serial_irq_2		4
#define serial_irq_3		3
#define serial_iobase(nr)	serial_iobase_##nr
#define serial_irq(nr)		serial_irq_##nr
#define SERIAL8250_BUS_TYPE	DEVICE_BUS_IOPORT
#endif

/*
 * This fakes a U6_16550A. The fifo len needs to be 64 as the kernel
 * expects that for autodetection.
 */
#define FIFO_LEN		64
#define FIFO_MASK		(FIFO_LEN - 1)

#define UART_IIR_TYPE_BITS	0xc0

struct serial8250_device {
	struct device_header	dev_hdr;
	struct mutex		mutex;
	u8			id;

	u32			iobase;
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

#ifdef CONFIG_HAS_LIBFDT
static
void serial8250_generate_fdt_node(void *fdt, struct device_header *dev_hdr,
				  fdt_irq_fn irq_fn);
#else
#define serial8250_generate_fdt_node	NULL
#endif
static struct serial8250_device devices[] = {
	/* ttyS0 */
	[0]	= {
		.dev_hdr = {
			.bus_type	= SERIAL8250_BUS_TYPE,
			.data		= serial8250_generate_fdt_node,
		},
		.mutex			= MUTEX_INITIALIZER,

		.id			= 0,
		.iobase			= serial_iobase(0),
		.irq			= serial_irq(0),

		SERIAL_REGS_SETTING
	},
	/* ttyS1 */
	[1]	= {
		.dev_hdr = {
			.bus_type	= SERIAL8250_BUS_TYPE,
			.data		= serial8250_generate_fdt_node,
		},
		.mutex			= MUTEX_INITIALIZER,

		.id			= 1,
		.iobase			= serial_iobase(1),
		.irq			= serial_irq(1),

		SERIAL_REGS_SETTING
	},
	/* ttyS2 */
	[2]	= {
		.dev_hdr = {
			.bus_type	= SERIAL8250_BUS_TYPE,
			.data		= serial8250_generate_fdt_node,
		},
		.mutex			= MUTEX_INITIALIZER,

		.id			= 2,
		.iobase			= serial_iobase(2),
		.irq			= serial_irq(2),

		SERIAL_REGS_SETTING
	},
	/* ttyS3 */
	[3]	= {
		.dev_hdr = {
			.bus_type	= SERIAL8250_BUS_TYPE,
			.data		= serial8250_generate_fdt_node,
		},
		.mutex			= MUTEX_INITIALIZER,

		.id			= 3,
		.iobase			= serial_iobase(3),
		.irq			= serial_irq(3),

		SERIAL_REGS_SETTING
	},
};

static void serial8250_flush_tx(struct kvm *kvm, struct serial8250_device *dev)
{
	dev->lsr |= UART_LSR_TEMT | UART_LSR_THRE;

	if (dev->txcnt) {
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

static bool serial8250_out(struct serial8250_device *dev, struct kvm_cpu *vcpu,
			   u16 offset, void *data)
{
	bool ret = true;
	char *addr = data;

	mutex_lock(&dev->mutex);

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
			serial8250_flush_tx(vcpu->kvm, dev);
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

	serial8250_update_irq(vcpu->kvm, dev);

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

static bool serial8250_in(struct serial8250_device *dev, struct kvm_cpu *vcpu,
			  u16 offset, void *data)
{
	bool ret = true;

	mutex_lock(&dev->mutex);

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

	serial8250_update_irq(vcpu->kvm, dev);

	mutex_unlock(&dev->mutex);

	return ret;
}

static void serial8250_mmio(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len,
			    u8 is_write, void *ptr)
{
	struct serial8250_device *dev = ptr;

	if (is_write)
		serial8250_out(dev, vcpu, addr - dev->iobase, data);
	else
		serial8250_in(dev, vcpu, addr - dev->iobase, data);
}

#ifdef CONFIG_HAS_LIBFDT

char *fdt_stdout_path = NULL;

#define DEVICE_NAME_MAX_LEN 32
static
void serial8250_generate_fdt_node(void *fdt, struct device_header *dev_hdr,
				  fdt_irq_fn irq_fn)
{
	char dev_name[DEVICE_NAME_MAX_LEN];
	struct serial8250_device *dev = container_of(dev_hdr,
						     struct serial8250_device,
						     dev_hdr);

	u64 addr = dev->iobase;
	u64 reg_prop[] = {
		cpu_to_fdt64(addr),
		cpu_to_fdt64(8),
	};

	snprintf(dev_name, DEVICE_NAME_MAX_LEN, "U6_16550A@%llx", addr);

	if (!fdt_stdout_path) {
		fdt_stdout_path = malloc(strlen(dev_name) + 2);
		/* Assumes that this node is a child of the root node. */
		sprintf(fdt_stdout_path, "/%s", dev_name);
	}

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property_string(fdt, "compatible", "ns16550a"));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	irq_fn(fdt, dev->irq, IRQ_TYPE_LEVEL_HIGH);
	_FDT(fdt_property_cell(fdt, "clock-frequency", 1843200));
	_FDT(fdt_end_node(fdt));
}
#endif

static int serial8250__device_init(struct kvm *kvm,
				   struct serial8250_device *dev)
{
	int r;

	r = device__register(&dev->dev_hdr);
	if (r < 0)
		return r;

	ioport__map_irq(&dev->irq);
	r = kvm__register_iotrap(kvm, dev->iobase, 8, serial8250_mmio, dev,
				 SERIAL8250_BUS_TYPE);

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

		kvm__deregister_iotrap(kvm, dev->iobase, SERIAL8250_BUS_TYPE);
		device__unregister(&dev->dev_hdr);
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

		r = kvm__deregister_iotrap(kvm, dev->iobase,
					   SERIAL8250_BUS_TYPE);
		if (r < 0)
			return r;
		device__unregister(&dev->dev_hdr);
	}

	return 0;
}
dev_exit(serial8250__exit);
