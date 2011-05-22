#include "kvm/ioport.h"

#include "kvm/kvm.h"
#include "kvm/util.h"
#include "kvm/rbtree-interval.h"

#include <linux/kvm.h>	/* for KVM_EXIT_* */
#include <linux/types.h>

#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>

#define ioport_node(n) rb_entry(n, struct ioport_entry, node)

struct ioport_entry {
	struct rb_int_node		node;
	struct ioport_operations	*ops;
};

static struct rb_root ioport_tree = RB_ROOT;
bool ioport_debug;

static struct ioport_entry *ioport_search(struct rb_root *root, u64 addr)
{
	struct rb_int_node *node;

	node = rb_int_search_single(root, addr);
	if (node == NULL)
		return NULL;

	return ioport_node(node);
}

static int ioport_insert(struct rb_root *root, struct ioport_entry *data)
{
	return rb_int_insert(root, &data->node);
}

static bool debug_io_out(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	exit(EXIT_SUCCESS);
}

static struct ioport_operations debug_ops = {
	.io_out		= debug_io_out,
};

static bool dummy_io_in(struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	return true;
}

static bool dummy_io_out(struct kvm *kvm, u16 port, void *data, int size, u32 count)
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

void ioport__register(u16 port, struct ioport_operations *ops, int count)
{
	struct ioport_entry *entry;

	entry = ioport_search(&ioport_tree, port);
	if (entry) {
		pr_warning("ioport re-registered: %x", port);
		rb_int_erase(&ioport_tree, &entry->node);
	}

	entry = malloc(sizeof(*entry));
	if (entry == NULL)
		die("Failed allocating new ioport entry");

	*entry = (struct ioport_entry) {
		.node	= RB_INT_INIT(port, port + count),
		.ops	= ops,
	};

	ioport_insert(&ioport_tree, entry);
}

static const char *to_direction(int direction)
{
	if (direction == KVM_EXIT_IO_IN)
		return "IN";
	else
		return "OUT";
}

static void ioport_error(u16 port, void *data, int direction, int size, u32 count)
{
	fprintf(stderr, "IO error: %s port=%x, size=%d, count=%u\n", to_direction(direction), port, size, count);
}

bool kvm__emulate_io(struct kvm *kvm, u16 port, void *data, int direction, int size, u32 count)
{
	struct ioport_operations *ops;
	bool ret;
	struct ioport_entry *entry;

	entry = ioport_search(&ioport_tree, port);
	if (!entry)
		goto error;

	ops = entry->ops;

	if (direction == KVM_EXIT_IO_IN) {
		if (!ops->io_in)
			goto error;

		ret = ops->io_in(kvm, port, data, size, count);
		if (!ret)
			goto error;
	} else {
		if (!ops->io_out)
			goto error;

		ret = ops->io_out(kvm, port, data, size, count);
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

	/* 0x00A0 - 0x00AF - 8259A PIC 2 */
	ioport__register(0x00A0, &dummy_read_write_ioport_ops, 2);

	/* PORT 00E0-00EF are 'motherboard specific' so we use them for our
	   internal debugging purposes.  */
	ioport__register(IOPORT_DBG, &debug_ops, 1);

	/* PORT 00ED - DUMMY PORT FOR DELAY??? */
	ioport__register(0x00ED, &dummy_write_only_ioport_ops, 1);

	/* 0x00F0 - 0x00FF - Math co-processor */
	ioport__register(0x00F0, &dummy_write_only_ioport_ops, 2);

	/* PORT 03D4-03D5 - COLOR VIDEO - CRT CONTROL REGISTERS */
	ioport__register(0x03D4, &dummy_read_write_ioport_ops, 1);
	ioport__register(0x03D5, &dummy_write_only_ioport_ops, 1);
}
