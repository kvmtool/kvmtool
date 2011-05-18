#include "kvm/disk-image.h"

/*
 * raw image and blk dev are similar, so reuse raw image ops.
 */
static struct disk_image_operations raw_image_ro_mmap_ops = {
	.read_sector		= raw_image__read_sector_ro_mmap,
	.write_sector		= raw_image__write_sector_ro_mmap,
	.close			= raw_image__close_ro_mmap,
};

struct disk_image *blkdev__probe(const char *filename, struct stat *st)
{
	u64 size;
	int fd;

	if (!S_ISBLK(st->st_mode))
		return NULL;

	fd		= open(filename, O_RDONLY);
	if (fd < 0)
		return NULL;

	if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
		close(fd);
		return NULL;
	}

	return disk_image__new(fd, size, &raw_image_ro_mmap_ops, DISK_IMAGE_MMAP);
}
