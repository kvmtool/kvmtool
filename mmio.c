#include "kvm/kvm.h"
#include "kvm/rbtree-interval.h"
#include "kvm/brlock.h"

#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/err.h>
#include <errno.h>

#define mmio_node(n) rb_entry(n, struct mmio_mapping, node)

struct mmio_mapping {
	struct rb_int_node	node;
	void			(*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr);
	void			*ptr;
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

int kvm__register_mmio(struct kvm *kvm, u64 phys_addr, u64 phys_addr_len, bool coalesce,
			void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
			void *ptr)
{
	struct mmio_mapping *mmio;
	struct kvm_coalesced_mmio_zone zone;
	int ret;

	mmio = malloc(sizeof(*mmio));
	if (mmio == NULL)
		return -ENOMEM;

	*mmio = (struct mmio_mapping) {
		.node = RB_INT_INIT(phys_addr, phys_addr + phys_addr_len),
		.mmio_fn = mmio_fn,
		.ptr	= ptr,
	};

	if (coalesce) {
		zone = (struct kvm_coalesced_mmio_zone) {
			.addr	= phys_addr,
			.size	= phys_addr_len,
		};
		ret = ioctl(kvm->vm_fd, KVM_REGISTER_COALESCED_MMIO, &zone);
		if (ret < 0) {
			free(mmio);
			return -errno;
		}
	}
	br_write_lock(kvm);
	ret = mmio_insert(&mmio_tree, mmio);
	br_write_unlock(kvm);

	return ret;
}

bool kvm__deregister_mmio(struct kvm *kvm, u64 phys_addr)
{
	struct mmio_mapping *mmio;
	struct kvm_coalesced_mmio_zone zone;

	br_write_lock(kvm);
	mmio = mmio_search_single(&mmio_tree, phys_addr);
	if (mmio == NULL) {
		br_write_unlock(kvm);
		return false;
	}

	zone = (struct kvm_coalesced_mmio_zone) {
		.addr	= phys_addr,
		.size	= 1,
	};
	ioctl(kvm->vm_fd, KVM_UNREGISTER_COALESCED_MMIO, &zone);

	rb_int_erase(&mmio_tree, &mmio->node);
	br_write_unlock(kvm);

	free(mmio);
	return true;
}

bool kvm__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write)
{
	struct mmio_mapping *mmio;

	br_read_lock();
	mmio = mmio_search(&mmio_tree, phys_addr, len);

	if (mmio)
		mmio->mmio_fn(phys_addr, data, len, is_write, mmio->ptr);
	else {
		if (kvm->cfg.mmio_debug)
			fprintf(stderr, "Warning: Ignoring MMIO %s at %016llx (length %u)\n",
				to_direction(is_write), phys_addr, len);
	}
	br_read_unlock();

	return true;
}
