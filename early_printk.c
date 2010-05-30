#include "kvm/early_printk.h"

#include "kvm/ioport.h"

#include <stdio.h>

static int early_serial_base = 0x3f8;  /* ttyS0 */

#define XMTRDY          0x20

#define TXR             0       /*  Transmit register (WRITE) */
#define LSR             5       /*  Line Status               */

static bool early_serial_txr_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	char *p = data;
	int i;

	while (count--) {
		for (i = 0; i < size; i++)
			fprintf(stderr, "%c", *p++);
	}
	fflush(stderr);

	return true;
}

static struct ioport_operations early_serial_txr_ops = {
	.io_out		= early_serial_txr_out,
};

static bool early_serial_lsr_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint8_t *p = data;

	*p	= XMTRDY;

	return true;
}

static struct ioport_operations early_serial_lsr_ops = {
	.io_in		= early_serial_lsr_in,
};

void early_printk__init(void)
{
	ioport__register(early_serial_base + TXR, &early_serial_txr_ops);
	ioport__register(early_serial_base + LSR, &early_serial_lsr_ops);
}
