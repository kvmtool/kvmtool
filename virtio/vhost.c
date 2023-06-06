#include "kvm/virtio.h"

#include <linux/kvm.h>
#include <linux/vhost.h>
#include <linux/list.h>

void virtio_vhost_init(struct kvm *kvm, int vhost_fd)
{
	struct kvm_mem_bank *bank;
	struct vhost_memory *mem;
	int i = 0, r;

	mem = calloc(1, sizeof(*mem) +
		     kvm->mem_slots * sizeof(struct vhost_memory_region));
	if (mem == NULL)
		die("Failed allocating memory for vhost memory map");

	list_for_each_entry(bank, &kvm->mem_banks, list) {
		mem->regions[i] = (struct vhost_memory_region) {
			.guest_phys_addr = bank->guest_phys_addr,
			.memory_size	 = bank->size,
			.userspace_addr	 = (unsigned long)bank->host_addr,
		};
		i++;
	}
	mem->nregions = i;

	r = ioctl(vhost_fd, VHOST_SET_OWNER);
	if (r != 0)
		die_perror("VHOST_SET_OWNER failed");

	r = ioctl(vhost_fd, VHOST_SET_MEM_TABLE, mem);
	if (r != 0)
		die_perror("VHOST_SET_MEM_TABLE failed");

	free(mem);
}

void virtio_vhost_set_vring(struct kvm *kvm, int vhost_fd, u32 index,
			    struct virt_queue *queue)
{
	int r;
	struct vhost_vring_addr addr = {
		.index = index,
		.desc_user_addr = (u64)(unsigned long)queue->vring.desc,
		.avail_user_addr = (u64)(unsigned long)queue->vring.avail,
		.used_user_addr = (u64)(unsigned long)queue->vring.used,
	};
	struct vhost_vring_state state = { .index = index };

	if (queue->endian != VIRTIO_ENDIAN_HOST)
		die("VHOST requires the same endianness in guest and host");

	state.num = queue->vring.num;
	r = ioctl(vhost_fd, VHOST_SET_VRING_NUM, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_NUM failed");

	state.num = 0;
	r = ioctl(vhost_fd, VHOST_SET_VRING_BASE, &state);
	if (r < 0)
		die_perror("VHOST_SET_VRING_BASE failed");

	r = ioctl(vhost_fd, VHOST_SET_VRING_ADDR, &addr);
	if (r < 0)
		die_perror("VHOST_SET_VRING_ADDR failed");
}

void virtio_vhost_set_vring_kick(struct kvm *kvm, int vhost_fd,
				 u32 index, int event_fd)
{
	int r;
	struct vhost_vring_file file = {
		.index	= index,
		.fd	= event_fd,
	};

	r = ioctl(vhost_fd, VHOST_SET_VRING_KICK, &file);
	if (r < 0)
		die_perror("VHOST_SET_VRING_KICK failed");
}
