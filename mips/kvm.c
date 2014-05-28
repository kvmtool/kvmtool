#include "kvm/kvm.h"
#include "kvm/ioport.h"
#include "kvm/virtio-console.h"

#include <linux/kvm.h>

#include <ctype.h>
#include <unistd.h>

struct kvm_ext kvm_req_ext[] = {
	{ 0, 0 }
};

void kvm__arch_read_term(struct kvm *kvm)
{
	virtio_console__inject_interrupt(kvm);
}

void kvm__init_ram(struct kvm *kvm)
{
	u64	phys_start, phys_size;
	void	*host_mem;

	phys_start = 0;
	phys_size  = kvm->ram_size;
	host_mem   = kvm->ram_start;

	kvm__register_mem(kvm, phys_start, phys_size, host_mem);
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	munmap(kvm->ram_start, kvm->ram_size);
}

void kvm__arch_set_cmdline(char *cmdline, bool video)
{

}

/* Architecture-specific KVM init */
void kvm__arch_init(struct kvm *kvm, const char *hugetlbfs_path, u64 ram_size)
{
	int ret;

	kvm->ram_start = mmap_anon_or_hugetlbfs(kvm, hugetlbfs_path, ram_size);
	kvm->ram_size = ram_size;

	if (kvm->ram_start == MAP_FAILED)
		die("out of memory");

	madvise(kvm->ram_start, kvm->ram_size, MADV_MERGEABLE);

	ret = ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP);
	if (ret < 0)
		die_perror("KVM_CREATE_IRQCHIP ioctl");
}

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	struct kvm_irq_level irq_level;
	int ret;

	irq_level.irq = irq;
	irq_level.level = level ? 1 : 0;

	ret = ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level);
	if (ret < 0)
		die_perror("KVM_IRQ_LINE ioctl");
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	struct kvm_irq_level irq_level;
	int ret;

	irq_level.irq = irq;
	irq_level.level = 1;

	ret = ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level);
	if (ret < 0)
		die_perror("KVM_IRQ_LINE ioctl");
}

void ioport__setup_arch(struct kvm *kvm)
{
}

bool kvm__arch_cpu_supports_vm(void)
{
	return true;
}
bool kvm__load_firmware(struct kvm *kvm, const char *firmware_filename)
{
	return false;
}
int kvm__arch_setup_firmware(struct kvm *kvm)
{
	return 0;
}

/* Load at the 1M point. */
#define KERNEL_LOAD_ADDR 0x1000000
int load_flat_binary(struct kvm *kvm, int fd_kernel, int fd_initrd, const char *kernel_cmdline)
{
	void *p;
	void *k_start;
	int nr;

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	p = k_start = guest_flat_to_host(kvm, KERNEL_LOAD_ADDR);

	while ((nr = read(fd_kernel, p, 65536)) > 0)
		p += nr;

	kvm->arch.is64bit = true;
	kvm->arch.entry_point = 0xffffffff81000000ull;

	pr_info("Loaded kernel to 0x%x (%ld bytes)", KERNEL_LOAD_ADDR, (long int)(p - k_start));

	return true;
}

void ioport__map_irq(u8 *irq)
{
}
