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

static void update_node_max_high(struct rb_node *node, void *arg)
{
	struct rb_int_node *i_node = rb_int(node);

	i_node->max_high = i_node->high;

	if (node->rb_left)
		i_node->max_high = max(i_node->max_high, rb_int(node->rb_left)->max_high);
	if (node->rb_right)
		i_node->max_high = max(i_node->max_high, rb_int(node->rb_right)->max_high);
}

int rb_int_insert(struct rb_root *root, struct rb_int_node *i_node)
{
	struct rb_node **node = &(root->rb_node), *parent = NULL;

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
	rb_insert_color(&i_node->node, root);

	rb_augment_insert(&i_node->node, update_node_max_high, NULL);
	return 0;
}

void rb_int_erase(struct rb_root *root, struct rb_int_node *node)
{
	struct rb_node *deepest;

	deepest = rb_augment_erase_begin(&node->node);
	rb_erase(&node->node, root);
	rb_augment_erase_end(deepest, update_node_max_high, NULL);

}
