#include "kvm/disk-image.h"

#include <linux/err.h>

/*
 * raw image and blk dev are similar, so reuse raw image ops.
 */
static struct disk_image_operations blk_dev_ops = {
	.read_sector		= raw_image__read_sector,
	.write_sector		= raw_image__write_sector,
	.close			= raw_image__close,
};

struct disk_image *blkdev__probe(const char *filename, struct stat *st)
{
	u64 size;
	int fd, r;

	if (!S_ISBLK(st->st_mode))
		return ERR_PTR(-EINVAL);

	/*
	 * Be careful! We are opening host block device!
	 * Open it readonly since we do not want to break user's data on disk.
	 */
	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return ERR_PTR(fd);

	if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
		r = -errno;
		close(fd);
		return ERR_PTR(r);
	}

	/*
	 * FIXME: This will not work on 32-bit host because we can not
	 * mmap large disk. There is not enough virtual address space
	 * in 32-bit host. However, this works on 64-bit host.
	 */
	return disk_image__new(fd, size, &blk_dev_ops, DISK_IMAGE_MMAP);
}
