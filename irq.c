#include "kvm/irq.h"

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>

#include <stddef.h>
#include <stdlib.h>

static u8		next_line	= 3;
static u8		next_dev	= 1;
static struct rb_root	pci_tree	= RB_ROOT;

static struct pci_dev *search(struct rb_root *root, u32 id)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct pci_dev *data = container_of(node, struct pci_dev, node);
		int result;

		result = id - data->id;

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static int insert(struct rb_root *root, struct pci_dev *data)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct pci_dev *this	= container_of(*new, struct pci_dev, node);
		int result		= data->id - this->id;

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			return 0;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);

	return 1;
}

int irq__register_device(u32 dev, u8 *num, u8 *pin, u8 *line)
{
	struct pci_dev *node;

	node = search(&pci_tree, dev);

	if (!node) {
		/* We haven't found a node - First device of it's kind */
		node = malloc(sizeof(*node));
		if (node == NULL)
			return -1;

		*node = (struct pci_dev) {
			.id	= dev,
			/*
			 * PCI supports only INTA#,B#,C#,D# per device.
			 * A#,B#,C#,D# are allowed for multifunctional
			 * devices so stick with A# for our single
			 * function devices.
			 */
			.pin	= 1,
		};

		INIT_LIST_HEAD(&node->lines);

		if (insert(&pci_tree, node) != 1) {
			free(node);
			return -1;
		}
	}

	if (node) {
		/* This device already has a pin assigned, give out a new line and device id */
		struct irq_line *new = malloc(sizeof(*new));
		if (new == NULL)
			return -1;

		new->line	= next_line++;
		*line		= new->line;
		*pin		= node->pin;
		*num		= next_dev++;

		list_add(&new->node, &node->lines);

		return 0;
	}

	return -1;
}

struct rb_node *irq__get_pci_tree(void)
{
	return rb_first(&pci_tree);
}
