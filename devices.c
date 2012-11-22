#include "kvm/devices.h"
#include "kvm/kvm.h"

#include <linux/err.h>
#include <linux/rbtree.h>

struct device_bus {
	struct rb_root	root;
	int		dev_num;
};

static struct device_bus device_trees[DEVICE_BUS_MAX] = {
	[0 ... (DEVICE_BUS_MAX - 1)] = { RB_ROOT, 0 },
};

int device__register(struct device_header *dev)
{
	struct device_bus *bus;
	struct rb_node **node, *parent = NULL;

	if (dev->bus_type >= DEVICE_BUS_MAX) {
		pr_warning("Ignoring device registration on unknown bus %d\n",
			   dev->bus_type);
		return -EINVAL;
	}

	bus = &device_trees[dev->bus_type];
	dev->dev_num = bus->dev_num++;

	node = &bus->root.rb_node;
	while (*node) {
		int num = rb_entry(*node, struct device_header, node)->dev_num;
		int result = dev->dev_num - num;

		if (result < 0)
			node = &((*node)->rb_left);
		else if (result > 0)
			node = &((*node)->rb_right);
		else
			return -EEXIST;
	}

	rb_link_node(&dev->node, parent, node);
	rb_insert_color(&dev->node, &bus->root);
	return 0;
}

struct device_header *device__find_dev(enum device_bus_type bus_type, u8 dev_num)
{
	struct rb_node *node;

	if (bus_type >= DEVICE_BUS_MAX)
		return ERR_PTR(-EINVAL);

	node = device_trees[bus_type].root.rb_node;
	while (node) {
		struct device_header *dev = rb_entry(node, struct device_header,
						     node);
		if (dev_num < dev->dev_num) {
			node = node->rb_left;
		} else if (dev_num > dev->dev_num) {
			node = node->rb_right;
		} else {
			return dev;
		}
	}

	return NULL;
}

struct device_header *device__first_dev(enum device_bus_type bus_type)
{
	struct rb_node *node;

	if (bus_type >= DEVICE_BUS_MAX)
		return NULL;

	node = rb_first(&device_trees[bus_type].root);
	return node ? rb_entry(node, struct device_header, node) : NULL;
}

struct device_header *device__next_dev(struct device_header *dev)
{
	struct rb_node *node = rb_next(&dev->node);
	return node ? rb_entry(node, struct device_header, node) : NULL;
}
