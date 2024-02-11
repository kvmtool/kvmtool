#include "kvm/kvm.h"
#include "kvm/boot-protocol.h"
#include "kvm/cpufeature.h"
#include "kvm/interrupt.h"
#include "kvm/mptable.h"
#include "kvm/util.h"
#include "kvm/8250-serial.h"
#include "kvm/virtio-console.h"
#include "kvm/irq.h"
#include "kvm/i8259.h"
#include "kvm/i8254.h"

#include <asm/bootparam.h>
#include <linux/kvm.h>
#include <linux/kernel.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

struct kvm_ext kvm_req_ext[] = {
	{ DEFINE_KVM_EXT(KVM_CAP_COALESCED_MMIO) },
	{ DEFINE_KVM_EXT(KVM_CAP_SET_TSS_ADDR) },
	{ DEFINE_KVM_EXT(KVM_CAP_PIT2) },
	{ DEFINE_KVM_EXT(KVM_CAP_USER_MEMORY) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQ_ROUTING) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQCHIP) },
	{ DEFINE_KVM_EXT(KVM_CAP_HLT) },
	{ DEFINE_KVM_EXT(KVM_CAP_IRQ_INJECT_STATUS) },
	{ DEFINE_KVM_EXT(KVM_CAP_EXT_CPUID) },
	{ 0, 0 }
};

u64 kvm__arch_default_ram_address(void)
{
	return 0;
}

void kvm__arch_validate_cfg(struct kvm *kvm)
{
}

bool kvm__arch_cpu_supports_vm(void)
{
	struct cpuid_regs regs;
	u32 eax_base;
	int feature;

	regs	= (struct cpuid_regs) {
		.eax		= 0x00,
	};
	host_cpuid(&regs);

	switch (regs.ebx) {
	case CPUID_VENDOR_INTEL_1:
		eax_base	= 0x00;
		feature		= KVM__X86_FEATURE_VMX;
		break;

	case CPUID_VENDOR_AMD_1:
		eax_base	= 0x80000000;
		feature		= KVM__X86_FEATURE_SVM;
		break;

	default:
		return false;
	}

	regs	= (struct cpuid_regs) {
		.eax		= eax_base,
	};
	host_cpuid(&regs);

	if (regs.eax < eax_base + 0x01)
		return false;

	regs	= (struct cpuid_regs) {
		.eax		= eax_base + 0x01
	};
	host_cpuid(&regs);

	return regs.ecx & (1 << feature);
}

/*
 * Allocating RAM size bigger than 4GB requires us to leave a gap
 * in the RAM which is used for PCI MMIO, hotplug, and unconfigured
 * devices (see documentation of e820_setup_gap() for details).
 *
 * If we're required to initialize RAM bigger than 4GB, we will create
 * a gap between 0xe0000000 and 0x100000000 in the guest virtual mem space.
 */

void kvm__init_ram(struct kvm *kvm)
{
	u64	phys_start, phys_size;
	void	*host_mem;

	if (kvm->ram_size < KVM_32BIT_GAP_START) {
		/* Use a single block of RAM for 32bit RAM */

		phys_start = 0;
		phys_size  = kvm->ram_size;
		host_mem   = kvm->ram_start;

		kvm__register_ram(kvm, phys_start, phys_size, host_mem);
	} else {
		/* First RAM range from zero to the PCI gap: */

		phys_start = 0;
		phys_size  = KVM_32BIT_GAP_START;
		host_mem   = kvm->ram_start;

		kvm__register_ram(kvm, phys_start, phys_size, host_mem);

		/* Second RAM range from 4GB to the end of RAM: */

		phys_start = KVM_32BIT_MAX_MEM_SIZE;
		phys_size  = kvm->ram_size - phys_start;
		host_mem   = kvm->ram_start + phys_start;

		kvm__register_ram(kvm, phys_start, phys_size, host_mem);
	}
}

/* Arch-specific commandline setup */
void kvm__arch_set_cmdline(char *cmdline, bool video)
{
	strcpy(cmdline, "noapic noacpi pci=conf1 reboot=k panic=1 i8042.direct=1 "
				"i8042.dumbkbd=1 i8042.nopnp=1");
	if (video)
		strcat(cmdline, " video=vesafb");
	else
		strcat(cmdline, " earlyprintk=serial i8042.noaux=1");
}

/* Architecture-specific KVM init */
void kvm__arch_init(struct kvm *kvm)
{
	const char *hugetlbfs_path = kvm->cfg.hugetlbfs_path;
	struct kvm_pit_config pit_config = { .flags = 0, };
	u64 ram_size = kvm->cfg.ram_size;
	int ret;

	ret = ioctl(kvm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000);
	if (ret < 0)
		die_perror("KVM_SET_TSS_ADDR ioctl");

	if (ram_size < KVM_32BIT_GAP_START) {
		kvm->ram_size = ram_size;
		kvm->ram_start = mmap_anon_or_hugetlbfs(kvm, hugetlbfs_path, ram_size);
	} else {
		kvm->ram_start = mmap_anon_or_hugetlbfs(kvm, hugetlbfs_path, ram_size + KVM_32BIT_GAP_SIZE);
		kvm->ram_size = ram_size + KVM_32BIT_GAP_SIZE;
		if (kvm->ram_start != MAP_FAILED)
			/*
			 * We mprotect the gap (see kvm__init_ram() for details) PROT_NONE so that
			 * if we accidently write to it, we will know.
			 */
			mprotect(kvm->ram_start + KVM_32BIT_GAP_START, KVM_32BIT_GAP_SIZE, PROT_NONE);
	}
	if (kvm->ram_start == MAP_FAILED)
		die("out of memory");

	madvise(kvm->ram_start, kvm->ram_size, MADV_MERGEABLE);

	if (!irqchip_split(kvm)) {
		ret = ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP);
		if (ret < 0)
			die_perror("KVM_CREATE_IRQCHIP ioctl");
	} else {
		struct kvm_enable_cap cap = {
			.cap = KVM_CAP_SPLIT_IRQCHIP,
			.flags = 0,
			.args[0] = 0,
		};
		ret = ioctl(kvm->vm_fd, KVM_ENABLE_CAP, &cap);
		if (ret < 0) {
			die_perror("Could not enable split irqchip mode");
		}
		kvm_pic_init(kvm);
	}

	if (!irqchip_split(kvm)) {
		ret = ioctl(kvm->vm_fd, KVM_CREATE_PIT2, &pit_config);
		if (ret < 0)
			die_perror("KVM_CREATE_PIT2 ioctl");
	} else {
		ret = pit_init(kvm);
		if (ret < 0)
			die_perror("pit_init");
	}
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	munmap(kvm->ram_start, kvm->ram_size);
}

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
	struct kvm_irq_level irq_level;

	if (irqchip_split(kvm)) {
		kvm_pic_set_irq(kvm->arch.vpic, irq, level);
		return;
	}

	irq_level	= (struct kvm_irq_level) {
		{
			.irq		= irq,
		},
		.level		= level,
	};

	if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
		die_perror("KVM_IRQ_LINE failed");
}

void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	kvm__irq_line(kvm, irq, 1);
	kvm__irq_line(kvm, irq, 0);
}

#define BOOT_LOADER_SELECTOR	0x1000
#define BOOT_LOADER_IP		0x0000
#define BOOT_LOADER_SP		0x8000
#define BOOT_CMDLINE_OFFSET	0x20000

#define BOOT_PROTOCOL_REQUIRED	0x206
#define LOAD_HIGH		0x01

static inline void *guest_real_to_host(struct kvm *kvm, u16 selector, u16 offset)
{
	unsigned long flat = ((u32)selector << 4) + offset;

	return guest_flat_to_host(kvm, flat);
}

static bool load_flat_binary(struct kvm *kvm, int fd_kernel)
{
	void *p;

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	p = guest_real_to_host(kvm, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);

	if (read_file(fd_kernel, p, kvm->cfg.ram_size) < 0)
		die_perror("read");

	kvm->arch.boot_selector	= BOOT_LOADER_SELECTOR;
	kvm->arch.boot_ip	= BOOT_LOADER_IP;
	kvm->arch.boot_sp	= BOOT_LOADER_SP;

	return true;
}

static const char *BZIMAGE_MAGIC = "HdrS";

static bool load_bzimage(struct kvm *kvm, int fd_kernel, int fd_initrd,
			 const char *kernel_cmdline)
{
	struct boot_params *kern_boot;
	struct boot_params boot;
	size_t cmdline_size;
	ssize_t file_size;
	void *p;
	u16 vidmode;

	/*
	 * See Documentation/x86/boot.txt for details no bzImage on-disk and
	 * memory layout.
	 */

	if (read_in_full(fd_kernel, &boot, sizeof(boot)) != sizeof(boot))
		return false;

	if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)))
		return false;

	if (boot.hdr.version < BOOT_PROTOCOL_REQUIRED)
		die("Too old kernel");

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	if (!boot.hdr.setup_sects)
		boot.hdr.setup_sects = BZ_DEFAULT_SETUP_SECTS;
	file_size = (boot.hdr.setup_sects + 1) << 9;
	p = guest_real_to_host(kvm, BOOT_LOADER_SELECTOR, BOOT_LOADER_IP);
	if (read_in_full(fd_kernel, p, file_size) != file_size)
		die_perror("kernel setup read");

	/* read actual kernel image (vmlinux.bin) to BZ_KERNEL_START */
	p = guest_flat_to_host(kvm, BZ_KERNEL_START);
	file_size = read_file(fd_kernel, p,
			      kvm->cfg.ram_size - BZ_KERNEL_START);
	if (file_size < 0)
		die_perror("kernel read");

	p = guest_flat_to_host(kvm, BOOT_CMDLINE_OFFSET);
	if (kernel_cmdline) {
		cmdline_size = strlen(kernel_cmdline) + 1;
		if (cmdline_size > boot.hdr.cmdline_size)
			cmdline_size = boot.hdr.cmdline_size;

		memset(p, 0, boot.hdr.cmdline_size);
		memcpy(p, kernel_cmdline, cmdline_size - 1);
	}

	/* vidmode should be either specified or set by default */
	if (kvm->cfg.vnc || kvm->cfg.sdl || kvm->cfg.gtk) {
		if (!kvm->cfg.arch.vidmode)
			vidmode = 0x312;
		else
			vidmode = kvm->cfg.arch.vidmode;
	} else {
		vidmode = 0;
	}

	kern_boot	= guest_real_to_host(kvm, BOOT_LOADER_SELECTOR, 0x00);

	kern_boot->hdr.cmd_line_ptr	= BOOT_CMDLINE_OFFSET;
	kern_boot->hdr.type_of_loader	= 0xff;
	kern_boot->hdr.heap_end_ptr	= 0xfe00;
	kern_boot->hdr.loadflags	|= CAN_USE_HEAP;
	kern_boot->hdr.vid_mode		= vidmode;

	/*
	 * Read initrd image into guest memory
	 */
	if (fd_initrd >= 0) {
		struct stat initrd_stat;
		unsigned long addr;

		if (fstat(fd_initrd, &initrd_stat))
			die_perror("fstat");

		addr = boot.hdr.initrd_addr_max & ~0xfffff;
		for (;;) {
			if (addr < BZ_KERNEL_START)
				die("Not enough memory for initrd");
			else if (addr < (kvm->ram_size - initrd_stat.st_size))
				break;
			addr -= 0x100000;
		}

		p = guest_flat_to_host(kvm, addr);
		if (read_in_full(fd_initrd, p, initrd_stat.st_size) < 0)
			die("Failed to read initrd");

		kern_boot->hdr.ramdisk_image	= addr;
		kern_boot->hdr.ramdisk_size	= initrd_stat.st_size;
	}

	kvm->arch.boot_selector = BOOT_LOADER_SELECTOR;
	/*
	 * The real-mode setup code starts at offset 0x200 of a bzImage. See
	 * Documentation/x86/boot.txt for details.
	 */
	kvm->arch.boot_ip = BOOT_LOADER_IP + 0x200;
	kvm->arch.boot_sp = BOOT_LOADER_SP;

	return true;
}

bool kvm__arch_load_kernel_image(struct kvm *kvm, int fd_kernel, int fd_initrd,
				 const char *kernel_cmdline)
{
	if (load_bzimage(kvm, fd_kernel, fd_initrd, kernel_cmdline))
		return true;
	pr_warning("Kernel image is not a bzImage.");
	pr_warning("Trying to load it as a flat binary (no cmdline support)");

	if (fd_initrd != -1)
		pr_warning("Loading initrd with flat binary not supported.");

	return load_flat_binary(kvm, fd_kernel);
}

/**
 * kvm__arch_setup_firmware - inject BIOS into guest system memory
 * @kvm - guest system descriptor
 *
 * This function is a main routine where we poke guest memory
 * and install BIOS there.
 */
int kvm__arch_setup_firmware(struct kvm *kvm)
{
	/* standart minimal configuration */
	setup_bios(kvm);

	/* FIXME: SMP, ACPI and friends here */

	return 0;
}

int kvm__arch_free_firmware(struct kvm *kvm)
{
	return 0;
}

void kvm__arch_read_term(struct kvm *kvm)
{
	serial8250__update_consoles(kvm);
	virtio_console__inject_interrupt(kvm);
}
