#include "kvm/kvm.h"

#include <asm/image.h>

#include <linux/byteorder.h>

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
	const char *warn_str;

	/* the 32bit kernel offset is a well known value */
	if (kvm->cfg.arch.aarch32_guest)
		return 0x8000;

	cur_offset = lseek(fd, 0, SEEK_CUR);
	if (cur_offset == (off_t)-1 ||
	    lseek(fd, 0, SEEK_SET) == (off_t)-1) {
		warn_str = "Failed to seek in kernel image file";
		goto fail;
	}

	size = xread(fd, &header, sizeof(header));
	if (size < 0 || (size_t)size < sizeof(header))
		die("Failed to read kernel image header");

	lseek(fd, cur_offset, SEEK_SET);

	if (memcmp(&header.magic, ARM64_IMAGE_MAGIC, sizeof(header.magic)))
		pr_warning("Kernel image magic not matching");

	if (le64_to_cpu(header.image_size))
		return le64_to_cpu(header.text_offset);

	warn_str = "Image size is 0";
fail:
	pr_warning("%s, assuming TEXT_OFFSET to be 0x80000", warn_str);
	return 0x80000;
}

