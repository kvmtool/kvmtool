#include "kvm/ioport.h"

#include "kvm/kvm.h"
#include "kvm/util.h"
#include "kvm/brlock.h"
#include "kvm/rbtree-interval.h"
#include "kvm/mutex.h"

#include <linux/kvm.h>	/* for KVM_EXIT_* */
#include <linux/types.h>

#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>

#define ioport_node(n) rb_entry(n, struct ioport, node)

DEFINE_MUTEX(ioport_mutex);

static u16			free_io_port_idx; /* protected by ioport_mutex */

static struct rb_root		ioport_tree = RB_ROOT;

static u16 ioport__find_free_port(void)
{
	u16 free_port;

	mutex_lock(&ioport_mutex);
	free_port = IOPORT_START + free_io_port_idx * IOPORT_SIZE;
	free_io_port_idx++;
	mutex_unlock(&ioport_mutex);

	return free_port;
}

static struct ioport *ioport_search(struct rb_root *root, u64 addr)
{
	struct rb_int_node *node;

	node = rb_int_search_single(root, addr);
	if (node == NULL)
		return NULL;

	return ioport_node(node);
}

static int ioport_insert(struct rb_root *root, struct ioport *data)
{
	return rb_int_insert(root, &data->node);
}

static void ioport_remove(struct rb_root *root, struct ioport *data)
{
	rb_int_erase(root, &data->node);
}

int ioport__register(struct kvm *kvm, u16 port, struct ioport_operations *ops, int count, void *param)
{
	struct ioport *entry;
	int r;

	br_write_lock(kvm);
	if (port == IOPORT_EMPTY)
		port = ioport__find_free_port();

	entry = ioport_search(&ioport_tree, port);
	if (entry) {
		pr_warning("ioport re-registered: %x", port);
		rb_int_erase(&ioport_tree, &entry->node);
	}

	entry = malloc(sizeof(*entry));
	if (entry == NULL)
		return -ENOMEM;

	*entry = (struct ioport) {
		.node	= RB_INT_INIT(port, port + count),
		.ops	= ops,
		.priv	= param,
	};

	r = ioport_insert(&ioport_tree, entry);
	if (r < 0) {
		free(entry);
		br_write_unlock(kvm);
		return r;
	}
	br_write_unlock(kvm);

	return port;
}

int ioport__unregister(struct kvm *kvm, u16 port)
{
	struct ioport *entry;
	int r;

	br_write_lock(kvm);

	r = -ENOENT;
	entry = ioport_search(&ioport_tree, port);
	if (!entry)
		goto done;

	ioport_remove(&ioport_tree, entry);

	free(entry);

	r = 0;

done:
	br_write_unlock(kvm);

	return r;
}

static void ioport__unregister_all(void)
{
	struct ioport *entry;
	struct rb_node *rb;
	struct rb_int_node *rb_node;

	rb = rb_first(&ioport_tree);
	while (rb) {
		rb_node = rb_int(rb);
		entry = ioport_node(rb_node);
		ioport_remove(&ioport_tree, entry);
		free(entry);
		rb = rb_first(&ioport_tree);
	}
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
	bool ret = false;
	struct ioport *entry;
	void *ptr = data;

	br_read_lock();
	entry = ioport_search(&ioport_tree, port);
	if (!entry)
		goto error;

	ops	= entry->ops;

	while (count--) {
		if (direction == KVM_EXIT_IO_IN && ops->io_in)
				ret = ops->io_in(entry, kvm, port, ptr, size);
		else if (ops->io_out)
				ret = ops->io_out(entry, kvm, port, ptr, size);

		ptr += size;
	}

	br_read_unlock();

	if (!ret)
		goto error;

	return true;
error:
	br_read_unlock();

	if (kvm->cfg.ioport_debug)
		ioport_error(port, data, direction, size, count);

	return !kvm->cfg.ioport_debug;
}

int ioport__init(struct kvm *kvm)
{
	ioport__setup_arch(kvm);

	return 0;
}
dev_base_init(ioport__init);

int ioport__exit(struct kvm *kvm)
{
	ioport__unregister_all();
	return 0;
}
dev_base_exit(ioport__exit);
