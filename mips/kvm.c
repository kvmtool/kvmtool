#include "kvm/8250-serial.h"
#include "kvm/kvm.h"
#include "kvm/ioport.h"
#include "kvm/virtio-console.h"

#include <linux/kvm.h>

#include <ctype.h>
#include <unistd.h>
#include <elf.h>

struct kvm_ext kvm_req_ext[] = {
	{ 0, 0 }
};

u64 kvm__arch_default_ram_address(void)
{
	return 0;
}

void kvm__arch_validate_cfg(struct kvm *kvm)
{
}

void kvm__arch_read_term(struct kvm *kvm)
{
	virtio_console__inject_interrupt(kvm);
}

void kvm__init_ram(struct kvm *kvm)
{
	u64	phys_start, phys_size;
	void	*host_mem;

	if (kvm->ram_size <= KVM_MMIO_START) {
		/* one region for all memory */
		phys_start = 0;
		phys_size  = kvm->ram_size;
		host_mem   = kvm->ram_start;

		kvm__register_ram(kvm, phys_start, phys_size, host_mem);
	} else {
		/* one region for memory that fits below MMIO range */
		phys_start = 0;
		phys_size  = KVM_MMIO_START;
		host_mem   = kvm->ram_start;

		kvm__register_ram(kvm, phys_start, phys_size, host_mem);

		/* one region for rest of memory */
		phys_start = KVM_MMIO_START + KVM_MMIO_SIZE;
		phys_size  = kvm->ram_size - KVM_MMIO_START;
		host_mem   = kvm->ram_start + KVM_MMIO_START;

		kvm__register_ram(kvm, phys_start, phys_size, host_mem);
	}
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	munmap(kvm->ram_start, kvm->ram_size);
}

void kvm__arch_set_cmdline(char *cmdline, bool video)
{

}

/* Architecture-specific KVM init */
void kvm__arch_init(struct kvm *kvm)
{
	int ret;

	kvm->ram_size = kvm->cfg.ram_size;
	kvm->ram_start = mmap_anon_or_hugetlbfs(kvm, kvm->cfg.hugetlbfs_path,
						kvm->ram_size);

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

static void kvm__mips_install_cmdline(struct kvm *kvm)
{
	char *p = kvm->ram_start;
	u64 cmdline_offset = 0x2000;
	u64 argv_start = 0x3000;
	u64 argv_offset = argv_start;
	u64 argc = 0;


	if ((u64) kvm->ram_size <= KVM_MMIO_START)
		sprintf(p + cmdline_offset, "mem=0x%llx@0 ",
			(unsigned long long)kvm->ram_size);
	else
		sprintf(p + cmdline_offset, "mem=0x%llx@0 mem=0x%llx@0x%llx ",
			(unsigned long long)KVM_MMIO_START,
			(unsigned long long)kvm->ram_size - KVM_MMIO_START,
			(unsigned long long)(KVM_MMIO_START + KVM_MMIO_SIZE));

	if (kvm->cfg.real_cmdline)
		strcat(p + cmdline_offset, kvm->cfg.real_cmdline); /* maximum size is 2K */

	while (p[cmdline_offset]) {
		if (!isspace(p[cmdline_offset])) {
			if (kvm->arch.is64bit) {
				*(u64 *)(p + argv_offset) = 0xffffffff80000000ull + cmdline_offset;
				argv_offset += sizeof(u64);
			} else {
				*(u32 *)(p + argv_offset) = 0x80000000u + cmdline_offset;
				argv_offset += sizeof(u32);
			}
			argc++;
			while(p[cmdline_offset] && !isspace(p[cmdline_offset]))
				cmdline_offset++;
			continue;
		}
		/* Must be a space character skip over these*/
		while(p[cmdline_offset] && isspace(p[cmdline_offset])) {
			p[cmdline_offset] = 0;
			cmdline_offset++;
		}
	}
	kvm->arch.argc = argc;
	kvm->arch.argv = 0xffffffff80000000ull + argv_start;
}

/* Load at the 1M point. */
#define KERNEL_LOAD_ADDR 0x1000000

static bool load_flat_binary(struct kvm *kvm, int fd_kernel)
{
	void *p;
	void *k_start;
	ssize_t kernel_size;

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	p = k_start = guest_flat_to_host(kvm, KERNEL_LOAD_ADDR);

	kernel_size = read_file(fd_kernel, p,
				kvm->cfg.ram_size - KERNEL_LOAD_ADDR);
	if (kernel_size == -1) {
		if (errno == ENOMEM)
			die("kernel too big for guest memory");
		else
			die_perror("kernel read");
	}

	kvm->arch.is64bit = true;
	kvm->arch.entry_point = 0xffffffff81000000ull;

	pr_info("Loaded kernel to 0x%x (%zd bytes)", KERNEL_LOAD_ADDR,
		kernel_size);

	return true;
}

struct kvm__arch_elf_info {
	u64 load_addr;
	u64 entry_point;
	size_t len;
	size_t offset;
};

static bool kvm__arch_get_elf_64_info(Elf64_Ehdr *ehdr, int fd_kernel,
				      struct kvm__arch_elf_info *ei)
{
	int i;
	Elf64_Phdr phdr;

	if (ehdr->e_phentsize != sizeof(phdr)) {
		pr_info("Incompatible ELF PHENTSIZE %d", ehdr->e_phentsize);
		return false;
	}

	ei->entry_point = ehdr->e_entry;

	if (lseek(fd_kernel, ehdr->e_phoff, SEEK_SET) < 0)
		die_perror("lseek");

	phdr.p_type = PT_NULL;
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (read_in_full(fd_kernel, &phdr, sizeof(phdr)) != sizeof(phdr)) {
			pr_info("Couldn't read %d bytes for ELF PHDR.", (int)sizeof(phdr));
			return false;
		}
		if (phdr.p_type == PT_LOAD)
			break;
	}
	if (phdr.p_type != PT_LOAD) {
		pr_info("No PT_LOAD Program Header found.");
		return false;
	}

	ei->load_addr = phdr.p_paddr;

	if ((ei->load_addr & 0xffffffffc0000000ull) == 0xffffffff80000000ull)
		ei->load_addr &= 0x1ffffffful; /* Convert KSEG{0,1} to physical. */
	if ((ei->load_addr & 0xc000000000000000ull) == 0x8000000000000000ull)
		ei->load_addr &= 0x07ffffffffffffffull; /* Convert XKPHYS to pysical */


	ei->len = phdr.p_filesz;
	ei->offset = phdr.p_offset;

	return true;
}

static bool kvm__arch_get_elf_32_info(Elf32_Ehdr *ehdr, int fd_kernel,
				      struct kvm__arch_elf_info *ei)
{
	int i;
	Elf32_Phdr phdr;

	if (ehdr->e_phentsize != sizeof(phdr)) {
		pr_info("Incompatible ELF PHENTSIZE %d", ehdr->e_phentsize);
		return false;
	}

	ei->entry_point = (s64)((s32)ehdr->e_entry);

	if (lseek(fd_kernel, ehdr->e_phoff, SEEK_SET) < 0)
		die_perror("lseek");

	phdr.p_type = PT_NULL;
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (read_in_full(fd_kernel, &phdr, sizeof(phdr)) != sizeof(phdr)) {
			pr_info("Couldn't read %d bytes for ELF PHDR.", (int)sizeof(phdr));
			return false;
		}
		if (phdr.p_type == PT_LOAD)
			break;
	}
	if (phdr.p_type != PT_LOAD) {
		pr_info("No PT_LOAD Program Header found.");
		return false;
	}

	ei->load_addr = (s64)((s32)phdr.p_paddr);

	if ((ei->load_addr & 0xffffffffc0000000ull) == 0xffffffff80000000ull)
		ei->load_addr &= 0x1fffffffull; /* Convert KSEG{0,1} to physical. */

	ei->len = phdr.p_filesz;
	ei->offset = phdr.p_offset;

	return true;
}

static bool load_elf_binary(struct kvm *kvm, int fd_kernel)
{
	union {
		Elf64_Ehdr ehdr;
		Elf32_Ehdr ehdr32;
	} eh;

	size_t nr;
	char *p;
	struct kvm__arch_elf_info ei;

	nr = read(fd_kernel, &eh, sizeof(eh));
	if (nr != sizeof(eh)) {
		pr_info("Couldn't read %d bytes for ELF header.", (int)sizeof(eh));
		return false;
	}

	if (eh.ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
	    eh.ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
	    eh.ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
	    eh.ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
	    (eh.ehdr.e_ident[EI_CLASS] != ELFCLASS64 && eh.ehdr.e_ident[EI_CLASS] != ELFCLASS32) ||
	    eh.ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
		pr_info("Incompatible ELF header.");
		return false;
	}
	if (eh.ehdr.e_type != ET_EXEC || eh.ehdr.e_machine != EM_MIPS) {
		pr_info("Incompatible ELF not MIPS EXEC.");
		return false;
	}

	if (eh.ehdr.e_ident[EI_CLASS] == ELFCLASS64) {
		if (!kvm__arch_get_elf_64_info(&eh.ehdr, fd_kernel, &ei))
			return false;
		kvm->arch.is64bit = true;
	} else {
		if (!kvm__arch_get_elf_32_info(&eh.ehdr32, fd_kernel, &ei))
			return false;
		kvm->arch.is64bit = false;
	}

	kvm->arch.entry_point = ei.entry_point;

	if (lseek(fd_kernel, ei.offset, SEEK_SET) < 0)
		die_perror("lseek");

	p = guest_flat_to_host(kvm, ei.load_addr);

	pr_info("ELF Loading 0x%lx bytes from 0x%llx to 0x%llx",
		(unsigned long)ei.len, (unsigned long long)ei.offset,
		(unsigned long long)ei.load_addr);

	if (read_in_full(fd_kernel, p, ei.len) != (ssize_t)ei.len)
		die_perror("read");

	return true;
}

bool kvm__arch_load_kernel_image(struct kvm *kvm, int fd_kernel, int fd_initrd,
				 const char *kernel_cmdline)
{
	if (fd_initrd != -1) {
		pr_err("Initrd not supported on MIPS.");
		return false;
	}

	if (load_elf_binary(kvm, fd_kernel)) {
		kvm__mips_install_cmdline(kvm);
		return true;
	}

	return load_flat_binary(kvm, fd_kernel);
}

void ioport__map_irq(u8 *irq)
{
}

void serial8250__inject_sysrq(struct kvm *kvm, char sysrq)
{
}
