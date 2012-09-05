#ifndef KVM__PCI_SHMEM_H
#define KVM__PCI_SHMEM_H

#include <linux/types.h>
#include <linux/list.h>

#include "kvm/parse-options.h"

#define SHMEM_DEFAULT_SIZE (16 << MB_SHIFT)
#define SHMEM_DEFAULT_ADDR (0xc8000000)
#define SHMEM_DEFAULT_HANDLE "/kvm_shmem"

struct kvm;
struct shmem_info;

struct shmem_info {
	u64 phys_addr;
	u64 size;
	char *handle;
	int create;
};

int pci_shmem__init(struct kvm *kvm);
int pci_shmem__exit(struct kvm *kvm);
int pci_shmem__register_mem(struct shmem_info *si);
int shmem_parser(const struct option *opt, const char *arg, int unset);

int pci_shmem__get_local_irqfd(struct kvm *kvm);
int pci_shmem__add_client(struct kvm *kvm, u32 id, int fd);
int pci_shmem__remove_client(struct kvm *kvm, u32 id);

#endif
