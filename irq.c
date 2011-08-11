#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

#include <stddef.h>
#include <stdlib.h>

#define IRQ_MAX_GSI			64
#define IRQCHIP_MASTER			0
#define IRQCHIP_SLAVE			1
#define IRQCHIP_IOAPIC			2

static u8		next_line	= 3;
static u8		next_dev	= 1;
static struct rb_root	pci_tree	= RB_ROOT;

/* First 24 GSIs are routed between IRQCHIPs and IOAPICs */
static u32 gsi = 24;

struct kvm_irq_routing *irq_routing;

static int irq__add_routing(u32 gsi, u32 type, u32 irqchip, u32 pin)
{
	if (gsi >= IRQ_MAX_GSI)
		return -ENOSPC;

	irq_routing->entries[irq_routing->nr++] =
		(struct kvm_irq_routing_entry) {
			.gsi = gsi,
			.type = type,
			.u.irqchip.irqchip = irqchip,
			.u.irqchip.pin = pin,
		};

	return 0;
}

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

void irq__init(struct kvm *kvm)
{
	int i, r;

	irq_routing = malloc(sizeof(struct kvm_irq_routing) +
			IRQ_MAX_GSI * sizeof(struct kvm_irq_routing_entry));
	if (irq_routing == NULL)
		die("Failed allocating space for GSI table");

	/* Hook first 8 GSIs to master IRQCHIP */
	for (i = 0; i < 8; i++)
		if (i != 2)
			irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_MASTER, i);

	/* Hook next 8 GSIs to slave IRQCHIP */
	for (i = 8; i < 16; i++)
		irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_SLAVE, i - 8);

	/* Last but not least, IOAPIC */
	for (i = 0; i < 24; i++) {
		if (i == 0)
			irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_IOAPIC, 2);
		else if (i != 2)
			irq__add_routing(i, KVM_IRQ_ROUTING_IRQCHIP, IRQCHIP_IOAPIC, i);
	}

	r = ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing);
	if (r)
		die("Failed setting GSI routes");
}

int irq__add_msix_route(struct kvm *kvm, u32 low, u32 high, u32 data)
{
	int r;

	irq_routing->entries[irq_routing->nr++] =
		(struct kvm_irq_routing_entry) {
			.gsi = gsi,
			.type = KVM_IRQ_ROUTING_MSI,
			.u.msi.address_lo = low,
			.u.msi.address_hi = high,
			.u.msi.data = data,
		};

	r = ioctl(kvm->vm_fd, KVM_SET_GSI_ROUTING, irq_routing);
	if (r)
		return r;

	return gsi++;
}

struct rb_node *irq__get_pci_tree(void)
{
	return rb_first(&pci_tree);
}
