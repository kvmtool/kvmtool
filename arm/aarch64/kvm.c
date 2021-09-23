#include "kvm/kvm.h"

#include <asm/image.h>

#include <linux/byteorder.h>
#include <kvm/util.h>

/*
 * Return the TEXT_OFFSET value that the guest kernel expects. Note
 * that pre-3.17 kernels expose this value using the native endianness
 * instead of Little-Endian. BE kernels of this vintage may fail to
 * boot. See Documentation/arm64/booting.rst in your local kernel tree.
 */
unsigned long long kvm__arch_get_kern_offset(struct kvm *kvm, int fd)
{
	struct arm64_image_header header;
	off_t cur_offset;
	ssize_t size;
	const char *debug_str;

	/* the 32bit kernel offset is a well known value */
	if (kvm->cfg.arch.aarch32_guest)
		return 0x8000;

	cur_offset = lseek(fd, 0, SEEK_CUR);
	if (cur_offset == (off_t)-1 ||
	    lseek(fd, 0, SEEK_SET) == (off_t)-1) {
		debug_str = "Failed to seek in kernel image file";
		goto default_offset;
	}

	size = xread(fd, &header, sizeof(header));
	if (size < 0 || (size_t)size < sizeof(header))
		die("Failed to read kernel image header");

	lseek(fd, cur_offset, SEEK_SET);

	if (memcmp(&header.magic, ARM64_IMAGE_MAGIC, sizeof(header.magic))) {
		debug_str = "Kernel image magic not matching";
		goto default_offset;
	}

	if (le64_to_cpu(header.image_size))
		return le64_to_cpu(header.text_offset);

	debug_str = "Image size is 0";
default_offset:
	pr_debug("%s, assuming TEXT_OFFSET to be 0x80000", debug_str);
	return 0x80000;
}

int kvm__arch_get_ipa_limit(struct kvm *kvm)
{
	int ret;

	ret = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_VM_IPA_SIZE);
	if (ret <= 0)
		ret = 0;

	return ret;
}

int kvm__get_vm_type(struct kvm *kvm)
{
	unsigned int ipa_bits, max_ipa_bits;
	unsigned long max_ipa;

	/* If we're running on an old kernel, use 0 as the VM type */
	max_ipa_bits = kvm__arch_get_ipa_limit(kvm);
	if (!max_ipa_bits)
		return 0;

	/* Otherwise, compute the minimal required IPA size */
	max_ipa = ARM_MEMORY_AREA + kvm->cfg.ram_size - 1;
	ipa_bits = max(32, fls_long(max_ipa));
	pr_debug("max_ipa %lx ipa_bits %d max_ipa_bits %d",
		 max_ipa, ipa_bits, max_ipa_bits);

	if (ipa_bits > max_ipa_bits)
		die("Memory too large for this system (needs %d bits, %d available)", ipa_bits, max_ipa_bits);

	return KVM_VM_TYPE_ARM_IPA_SIZE(ipa_bits);
}
