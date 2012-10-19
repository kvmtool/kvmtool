#include <kvm/rbtree-interval.h>
#include <stddef.h>
#include <errno.h>

struct rb_int_node *rb_int_search_single(struct rb_root *root, u64 point)
{
	struct rb_node *node = root->rb_node;
	struct rb_node *lowest = NULL;

	while (node) {
		struct rb_int_node *cur = rb_int(node);

		if (node->rb_left && (rb_int(node->rb_left)->max_high > point)) {
			node = node->rb_left;
		} else if (cur->low <= point && cur->high > point) {
			lowest = node;
			break;
		} else if (point > cur->low) {
			node = node->rb_right;
		} else {
			break;
		}
	}

	if (lowest == NULL)
		return NULL;

	return rb_int(lowest);
}

struct rb_int_node *rb_int_search_range(struct rb_root *root, u64 low, u64 high)
{
	struct rb_int_node *range;

	range = rb_int_search_single(root, low);
	if (range == NULL)
		return NULL;

	/* We simply verify that 'high' is smaller than the end of the range where 'low' is located */
	if (range->high < high)
		return NULL;

	return range;
}

/*
 * Update a node after it has been linked into the tree:
 */
static void propagate_callback(struct rb_node *node, struct rb_node *stop)
{
	struct rb_int_node *i_node = rb_int(node);

	i_node->max_high = i_node->high;

	if (node->rb_left)
		i_node->max_high = max(i_node->max_high, rb_int(node->rb_left)->max_high);
	if (node->rb_right)
		i_node->max_high = max(i_node->max_high, rb_int(node->rb_right)->max_high);
}

/*
 * Copy the extra data to a new node:
 */
static void copy_callback(struct rb_node *node_old, struct rb_node *node_new)
{
	struct rb_int_node *i_node_old = rb_int(node_old);
	struct rb_int_node *i_node_new = rb_int(node_new);

	i_node_new->low		= i_node_old->low;
	i_node_new->high	= i_node_old->high;

	i_node_new->max_high	= i_node_old->max_high;
}

/*
 * Update after tree rotation:
 */
static void rotate_callback(struct rb_node *node_old, struct rb_node *node_new)
{
	propagate_callback(node_old, NULL);
	propagate_callback(node_new, NULL);
}

/*
 * All augmented rbtree callbacks:
 */
struct rb_augment_callbacks callbacks = {
	.propagate	= propagate_callback,
	.copy		= copy_callback,
	.rotate		= rotate_callback,
};

int rb_int_insert(struct rb_root *root, struct rb_int_node *i_node)
{
	struct rb_node **node = &root->rb_node, *parent = NULL;

	while (*node) {
		int result = i_node->low - rb_int(*node)->low;

		parent = *node;
		if (result < 0)
			node	= &((*node)->rb_left);
		else if (result > 0)
			node	= &((*node)->rb_right);
		else
			return -EEXIST;
	}

	rb_link_node(&i_node->node, parent, node);
	rb_insert_augmented(&i_node->node, root, &callbacks);

	return 0;
}

void rb_int_erase(struct rb_root *root, struct rb_int_node *node)
{
	rb_erase_augmented(&node->node, root, &callbacks);
}
