#ifndef KVM__INTERVAL_RBTREE_H
#define KVM__INTERVAL_RBTREE_H

#include <linux/rbtree.h>
#include <linux/types.h>

#define RB_INT_INIT(l, h) \
	(struct rb_int_node){.low = l, .high = h}
#define rb_int(n) rb_entry(n, struct rb_int_node, node)

struct rb_int_node {
	struct rb_node	node;
	u64		low;
	u64		high;
};

/* Return the rb_int_node interval in which 'point' is located. */
struct rb_int_node *rb_int_search_single(struct rb_root *root, u64 point);

/* Return the rb_int_node in which start:len is located. */
struct rb_int_node *rb_int_search_range(struct rb_root *root, u64 low, u64 high);

int rb_int_insert(struct rb_root *root, struct rb_int_node *data);

static inline void rb_int_erase(struct rb_root *root, struct rb_int_node *node)
{
	rb_erase(&node->node, root);
}

#endif
