#ifndef KVM__DEVICES_H
#define KVM__DEVICES_H

#include <linux/rbtree.h>
#include <linux/types.h>

enum device_bus_type {
	DEVICE_BUS_PCI,
	DEVICE_BUS_MMIO,
	DEVICE_BUS_MAX,
};

struct device_header {
	enum device_bus_type	bus_type;
	void			*data;
	int			dev_num;
	struct rb_node		node;
};

int device__register(struct device_header *dev);
struct device_header *device__find_dev(enum device_bus_type bus_type,
				       u8 dev_num);

struct device_header *device__first_dev(enum device_bus_type bus_type);
struct device_header *device__next_dev(struct device_header *dev);

#endif /* KVM__DEVICES_H */
