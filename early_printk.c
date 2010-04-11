#include "kvm/early_printk.h"

#include "kvm/ioport.h"

#include <stdio.h>

static bool early_serial_txr_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	char *p = data;

	printf("%c", *p);

	return true;
}

static struct ioport_operations early_serial_txr_ops = {
	.io_out		= early_serial_txr_out,
};

static bool early_serial_lsr_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint8_t *p = data;

	*p	= 0x20;	/* xmtrdy */

	return true;
}

static struct ioport_operations early_serial_lsr_ops = {
	.io_in		= early_serial_lsr_in,
};

void early_printk__init(void)
{
	ioport__register(0x03F8, &early_serial_txr_ops);
	ioport__register(0x03FD, &early_serial_lsr_ops);
}
