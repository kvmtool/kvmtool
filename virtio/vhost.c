#include <linux/kvm.h>
#include <linux/vhost.h>
#include <linux/list.h>
#include "kvm/virtio.h"

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
