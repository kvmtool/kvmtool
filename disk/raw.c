#include "kvm/disk-image.h"

#include <linux/err.h>

ssize_t raw_image__read_sync(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	return preadv_in_full(disk->fd, iov, iovcount, sector << SECTOR_SHIFT);
}

ssize_t raw_image__write_sync(struct disk_image *disk, u64 sector,
			      const struct iovec *iov, int iovcount,
			      void *param)
{
	return pwritev_in_full(disk->fd, iov, iovcount, sector << SECTOR_SHIFT);
}

ssize_t raw_image__read_mmap(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	u64 offset = sector << SECTOR_SHIFT;
	ssize_t total = 0;

	while (iovcount--) {
		memcpy(iov->iov_base, disk->priv + offset, iov->iov_len);

		sector	+= iov->iov_len >> SECTOR_SHIFT;
		offset	+= iov->iov_len;
		total	+= iov->iov_len;
		iov++;
	}

	return total;
}

ssize_t raw_image__write_mmap(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	u64 offset = sector << SECTOR_SHIFT;
	ssize_t total = 0;

	while (iovcount--) {
		memcpy(disk->priv + offset, iov->iov_base, iov->iov_len);

		sector	+= iov->iov_len >> SECTOR_SHIFT;
		offset	+= iov->iov_len;
		total	+= iov->iov_len;
		iov++;
	}

	return total;
}

int raw_image__close(struct disk_image *disk)
{
	int ret = 0;

	if (disk->priv != MAP_FAILED)
		ret = munmap(disk->priv, disk->size);

	return ret;
}

/*
 * multiple buffer based disk image operations
 */
static struct disk_image_operations raw_image_regular_ops = {
	.read	= raw_image__read,
	.write	= raw_image__write,
	.wait	= raw_image__wait,
	.async	= true,
};

struct disk_image_operations ro_ops = {
	.read	= raw_image__read_mmap,
	.write	= raw_image__write_mmap,
	.close	= raw_image__close,
};

struct disk_image_operations ro_ops_nowrite = {
	.read	= raw_image__read,
	.wait	= raw_image__wait,
	.async	= true,
};

struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly)
{
	if (readonly) {
		/*
		 * Use mmap's MAP_PRIVATE to implement non-persistent write
		 * FIXME: This does not work on 32-bit host.
		 */
		struct disk_image *disk;

		disk = disk_image__new(fd, st->st_size, &ro_ops, DISK_IMAGE_MMAP);
		if (IS_ERR_OR_NULL(disk)) {
			disk = disk_image__new(fd, st->st_size, &ro_ops_nowrite, DISK_IMAGE_REGULAR);
		}

		return disk;
	} else {
		/*
		 * Use read/write instead of mmap
		 */
		return disk_image__new(fd, st->st_size, &raw_image_regular_ops, DISK_IMAGE_REGULAR);
	}
}
