#include "kvm/ioport.h"

#include "kvm/kvm.h"

#include <linux/kvm.h>	/* for KVM_EXIT_* */

#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>

bool ioport_debug;

static uint8_t ioport_to_uint8(void *data)
{
	uint8_t *p = data;

	return *p;
}

static bool cmos_ram_rtc_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	uint8_t value;

	value	= ioport_to_uint8(data);

	self->nmi_disabled	= value & (1UL << 7);

	return true;
}

static struct ioport_operations cmos_ram_rtc_ops = {
	.io_out		= cmos_ram_rtc_io_out,
};

static bool debug_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	exit(EXIT_SUCCESS);
}

static struct ioport_operations debug_ops = {
	.io_out		= debug_io_out,
};

static bool dummy_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	return true;
}

static bool dummy_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
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

static struct ioport_operations *ioport_ops[USHRT_MAX];

void ioport__register(uint16_t port, struct ioport_operations *ops, int count)
{
	int i;

	for (i = 0; i < count; i++)
		ioport_ops[port + i]	= ops;
}

static const char *to_direction(int direction)
{
	if (direction == KVM_EXIT_IO_IN)
		return "IN";
	else
		return "OUT";
}

static void ioport_error(uint16_t port, void *data, int direction, int size, uint32_t count)
{
	fprintf(stderr, "IO error: %s port=%x, size=%d, count=%" PRIu32 "\n", to_direction(direction), port, size, count);
}

bool kvm__emulate_io(struct kvm *self, uint16_t port, void *data, int direction, int size, uint32_t count)
{
	struct ioport_operations *ops = ioport_ops[port];
	bool ret;

	if (!ops)
		goto error;

	if (direction == KVM_EXIT_IO_IN) {
		if (!ops->io_in)
			goto error;

		ret = ops->io_in(self, port, data, size, count);
		if (!ret)
			goto error;
	} else {
		if (!ops->io_out)
			goto error;

		ret = ops->io_out(self, port, data, size, count);
		if (!ret)
			goto error;
	}
	return true;
error:
	if (ioport_debug)
		ioport_error(port, data, direction, size, count);

	return !ioport_debug;
}

void ioport__setup_legacy(void)
{
	/* 0x0020 - 0x003F - 8259A PIC 1 */
	ioport__register(0x0020, &dummy_read_write_ioport_ops, 2);

	/* PORT 0040-005F - PIT - PROGRAMMABLE INTERVAL TIMER (8253, 8254) */
	ioport__register(0x0040, &dummy_read_write_ioport_ops, 4);

	/* PORT 0060-006F - KEYBOARD CONTROLLER 804x (8041, 8042) (or PPI (8255) on PC,XT) */
	ioport__register(0x0060, &dummy_read_write_ioport_ops, 2);
	ioport__register(0x0064, &dummy_read_write_ioport_ops, 1);

	/* PORT 0070-007F - CMOS RAM/RTC (REAL TIME CLOCK) */
	ioport__register(0x0070, &cmos_ram_rtc_ops, 1);
	ioport__register(0x0071, &dummy_read_write_ioport_ops, 1);

	/* 0x00A0 - 0x00AF - 8259A PIC 2 */
	ioport__register(0x00A0, &dummy_read_write_ioport_ops, 2);

	/* PORT 00E0-00EF are 'motherboard specific' so we use them for our
	   internal debugging purposes.  */
	ioport__register(IOPORT_DBG, &debug_ops, 1);

	/* PORT 00ED - DUMMY PORT FOR DELAY??? */
	ioport__register(0x00ED, &dummy_write_only_ioport_ops, 1);

	/* 0x00F0 - 0x00FF - Math co-processor */
	ioport__register(0x00F0, &dummy_write_only_ioport_ops, 2);

	/* PORT 02E8-02EF - serial port, same as 02F8, 03E8 and 03F8 (COM4) */
	ioport__register(0x02E8, &dummy_read_write_ioport_ops, 7);

	/* PORT 02F8-02FF - serial port, same as 02E8, 03E8 and 03F8 (COM2) */
	ioport__register(0x02F8, &dummy_read_write_ioport_ops, 7);

	/* PORT 03D4-03D5 - COLOR VIDEO - CRT CONTROL REGISTERS */
	ioport__register(0x03D4, &dummy_read_write_ioport_ops, 1);
	ioport__register(0x03D5, &dummy_write_only_ioport_ops, 1);

	/* PORT 03E8-03EF - serial port, same as 02E8, 02F8 and 03F8 (COM3) */
	ioport__register(0x03E8, &dummy_read_write_ioport_ops, 7);

	/* PORT 03F8-03FF - Serial port (8250,8250A,8251,16450,16550,16550A,etc.) COM1 */
	ioport__register(0x03F8, &dummy_read_write_ioport_ops, 7);

	/* PORT 0CF8-0CFF - PCI Configuration Mechanism 1 - Configuration Registers */
	ioport__register(0x0CF8, &dummy_write_only_ioport_ops, 1);
	ioport__register(0x0CFC, &dummy_read_write_ioport_ops, 1);
	ioport__register(0x0CFE, &dummy_read_write_ioport_ops, 1);
}
