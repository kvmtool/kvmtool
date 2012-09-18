#include "kvm/ioport.h"

#include <stdlib.h>
#include <stdio.h>

static bool debug_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	return 0;
}

static struct ioport_operations debug_ops = {
	.io_out		= debug_io_out,
};

static bool seabios_debug_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	char ch;

	ch = ioport__read8(data);

	putchar(ch);

	return true;
}

static struct ioport_operations seabios_debug_ops = {
	.io_out		= seabios_debug_io_out,
};

static bool dummy_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	return true;
}

static bool dummy_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size)
{
	return true;
}

static struct ioport_operations dummy_read_write_ioport_ops = {
	.io_in		= dummy_io_in,
	.io_out		= dummy_io_out,
};

static struct ioport_operations dummy_write_only_ioport_ops = {
	.io_out		= dummy_io_out,
};

void ioport__setup_arch(struct kvm *kvm)
{
	/* Legacy ioport setup */

	/* 0x0020 - 0x003F - 8259A PIC 1 */
	ioport__register(kvm, 0x0020, &dummy_read_write_ioport_ops, 2, NULL);

	/* PORT 0040-005F - PIT - PROGRAMMABLE INTERVAL TIMER (8253, 8254) */
	ioport__register(kvm, 0x0040, &dummy_read_write_ioport_ops, 4, NULL);

	/* 0x00A0 - 0x00AF - 8259A PIC 2 */
	ioport__register(kvm, 0x00A0, &dummy_read_write_ioport_ops, 2, NULL);

	/* PORT 00E0-00EF are 'motherboard specific' so we use them for our
	   internal debugging purposes.  */
	ioport__register(kvm, IOPORT_DBG, &debug_ops, 1, NULL);

	/* PORT 00ED - DUMMY PORT FOR DELAY??? */
	ioport__register(kvm, 0x00ED, &dummy_write_only_ioport_ops, 1, NULL);

	/* 0x00F0 - 0x00FF - Math co-processor */
	ioport__register(kvm, 0x00F0, &dummy_write_only_ioport_ops, 2, NULL);

	/* PORT 03D4-03D5 - COLOR VIDEO - CRT CONTROL REGISTERS */
	ioport__register(kvm, 0x03D4, &dummy_read_write_ioport_ops, 1, NULL);
	ioport__register(kvm, 0x03D5, &dummy_write_only_ioport_ops, 1, NULL);

	ioport__register(kvm, 0x402, &seabios_debug_ops, 1, NULL);
}
