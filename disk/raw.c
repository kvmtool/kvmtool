#include "kvm/disk-image.h"

ssize_t raw_image__read_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	u64 offset = sector << SECTOR_SHIFT;

	return preadv_in_full(disk->fd, iov, iovcount, offset);
}

ssize_t raw_image__write_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	u64 offset = sector << SECTOR_SHIFT;

	return pwritev_in_full(disk->fd, iov, iovcount, offset);
}

ssize_t raw_image__read_sector(struct disk_image *disk, u64 sector, void *dst, u32 dst_len)
{
	u64 offset = sector << SECTOR_SHIFT;

	if (offset + dst_len > disk->size)
		return -1;

	memcpy(dst, disk->priv + offset, dst_len);

	return dst_len;
}

ssize_t raw_image__write_sector(struct disk_image *disk, u64 sector, void *src, u32 src_len)
{
	u64 offset = sector << SECTOR_SHIFT;

	if (offset + src_len > disk->size)
		return -1;

	memcpy(disk->priv + offset, src, src_len);

	return src_len;
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
static struct disk_image_operations raw_image_iov_ops = {
	.read_sector_iov	= raw_image__read_sector_iov,
	.write_sector_iov	= raw_image__write_sector_iov,
};

/*
 * single buffer based disk image operations
 */
static struct disk_image_operations raw_image_ops = {
	.read_sector		= raw_image__read_sector,
	.write_sector		= raw_image__write_sector,
	.close			= raw_image__close,
};

struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly)
{

	if (readonly)
		/*
		 * Use mmap's MAP_PRIVATE to implement non-persistent write
		 * FIXME: This does not work on 32-bit host.
		 */
		return disk_image__new(fd, st->st_size, &raw_image_ops, DISK_IMAGE_MMAP);
	else
		/*
		 * Use read/write instead of mmap
		 */
		return disk_image__new(fd, st->st_size, &raw_image_iov_ops, DISK_IMAGE_NOMMAP);
}
