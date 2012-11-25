#include <kvm/rbtree-interval.h>
#include <stddef.h>
#include <errno.h>

struct rb_int_node *rb_int_search_single(struct rb_root *root, u64 point)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct rb_int_node *cur = rb_int(node);

		if (point < cur->low)
			node = node->rb_left;
		else if (cur->high <= point)
			node = node->rb_right;
		else
			return cur;
	}

	return NULL;
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

int rb_int_insert(struct rb_root *root, struct rb_int_node *i_node)
{
	struct rb_node **node = &root->rb_node, *parent = NULL;

	while (*node) {
		struct rb_int_node *cur = rb_int(*node);

		parent = *node;
		if (i_node->high <= cur->low)
			node = &cur->node.rb_left;
		else if (cur->high <= i_node->low)
			node = &cur->node.rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&i_node->node, parent, node);
	rb_insert_color(&i_node->node, root);

	return 0;
}
