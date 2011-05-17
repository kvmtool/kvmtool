#include "kvm/kvm.h"
#include "kvm/rbtree-interval.h"

#include <stdio.h>
#include <stdlib.h>

#include <linux/types.h>
#include <linux/rbtree.h>

#define mmio_node(n) rb_entry(n, struct mmio_mapping, node)

struct mmio_mapping {
	struct rb_int_node	node;
	void			(*kvm_mmio_callback_fn)(u64 addr, u8 *data, u32 len, u8 is_write);
};

static struct rb_root mmio_tree = RB_ROOT;

static struct mmio_mapping *mmio_search(struct rb_root *root, u64 addr, u64 len)
{
	struct rb_int_node *node;

	node = rb_int_search_range(root, addr, addr + len);
	if (node == NULL)
		return NULL;

	return mmio_node(node);
}

/* Find lowest match, Check for overlap */
static struct mmio_mapping *mmio_search_single(struct rb_root *root, u64 addr)
{
	struct rb_int_node *node;

	node = rb_int_search_single(root, addr);
	if (node == NULL)
		return NULL;

	return mmio_node(node);
}

static int mmio_insert(struct rb_root *root, struct mmio_mapping *data)
{
	return rb_int_insert(root, &data->node);
}

static const char *to_direction(u8 is_write)
{
	if (is_write)
		return "write";

	return "read";
}

bool kvm__register_mmio(u64 phys_addr, u64 phys_addr_len, void (*kvm_mmio_callback_fn)(u64 addr, u8 *data, u32 len, u8 is_write))
{
	struct mmio_mapping *mmio;

	mmio = malloc(sizeof(*mmio));
	if (mmio == NULL)
		return false;

	*mmio = (struct mmio_mapping) {
		.node = RB_INT_INIT(phys_addr, phys_addr + phys_addr_len),
		.kvm_mmio_callback_fn = kvm_mmio_callback_fn,
	};

	return mmio_insert(&mmio_tree, mmio);
}

bool kvm__deregister_mmio(u64 phys_addr)
{
	struct mmio_mapping *mmio;

	mmio = mmio_search_single(&mmio_tree, phys_addr);
	if (mmio == NULL)
		return false;

	rb_int_erase(&mmio_tree, &mmio->node);
	free(mmio);
	return true;
}

bool kvm__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write)
{
	struct mmio_mapping *mmio = mmio_search(&mmio_tree, phys_addr, len);

	if (mmio)
		mmio->kvm_mmio_callback_fn(phys_addr, data, len, is_write);
	else
		fprintf(stderr, "Warning: Ignoring MMIO %s at %016llx (length %u)\n",
			to_direction(is_write), phys_addr, len);

	return true;
}
