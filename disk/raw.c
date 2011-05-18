#include "kvm/disk-image.h"

static ssize_t raw_image__read_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	u64 offset = sector << SECTOR_SHIFT;

	return preadv_in_full(disk->fd, iov, iovcount, offset);
}

static ssize_t raw_image__write_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	u64 offset = sector << SECTOR_SHIFT;

	return pwritev_in_full(disk->fd, iov, iovcount, offset);
}

static int raw_image__read_sector_ro_mmap(struct disk_image *disk, u64 sector, void *dst, u32 dst_len)
{
	u64 offset = sector << SECTOR_SHIFT;

	if (offset + dst_len > disk->size)
		return -1;

	memcpy(dst, disk->priv + offset, dst_len);

	return 0;
}

static int raw_image__write_sector_ro_mmap(struct disk_image *disk, u64 sector, void *src, u32 src_len)
{
	u64 offset = sector << SECTOR_SHIFT;

	if (offset + src_len > disk->size)
		return -1;

	memcpy(disk->priv + offset, src, src_len);

	return 0;
}

static void raw_image__close_ro_mmap(struct disk_image *disk)
{
	if (disk->priv != MAP_FAILED)
		munmap(disk->priv, disk->size);
}

static struct disk_image_operations raw_image_ops = {
	.read_sector_iov	= raw_image__read_sector_iov,
	.write_sector_iov	= raw_image__write_sector_iov
};

static struct disk_image_operations raw_image_ro_mmap_ops = {
	.read_sector		= raw_image__read_sector_ro_mmap,
	.write_sector		= raw_image__write_sector_ro_mmap,
	.close			= raw_image__close_ro_mmap,
};

struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly)
{
	if (readonly)
		return disk_image__new_readonly(fd, st->st_size, &raw_image_ro_mmap_ops);
	else
		return disk_image__new(fd, st->st_size, &raw_image_ops);
}

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

	return disk_image__new_readonly(fd, size, &raw_image_ro_mmap_ops);
}
