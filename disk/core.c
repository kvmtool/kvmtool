#include "kvm/disk-image.h"

#include "kvm/read-write.h"
#include "kvm/qcow.h"
#include "kvm/util.h"

#include <linux/fs.h>	/* for BLKGETSIZE64 */

#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

struct disk_image *disk_image__new(int fd, u64 size, struct disk_image_operations *ops)
{
	struct disk_image *disk;

	disk		= malloc(sizeof *disk);
	if (!disk)
		return NULL;

	disk->fd	= fd;
	disk->size	= size;
	disk->ops	= ops;
	return disk;
}

struct disk_image *disk_image__new_readonly(int fd, u64 size, struct disk_image_operations *ops)
{
	struct disk_image *disk;

	disk = disk_image__new(fd, size, ops);
	if (!disk)
		return NULL;

	disk->priv = mmap(NULL, size, PROT_RW, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
	if (disk->priv == MAP_FAILED)
		die("mmap() failed");
	return disk;
}

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

static struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly)
{
	if (readonly)
		return disk_image__new_readonly(fd, st->st_size, &raw_image_ro_mmap_ops);
	else
		return disk_image__new(fd, st->st_size, &raw_image_ops);
}

static struct disk_image *blkdev__probe(const char *filename, struct stat *st)
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

struct disk_image *disk_image__open(const char *filename, bool readonly)
{
	struct disk_image *disk;
	struct stat st;
	int fd;

	if (stat(filename, &st) < 0)
		return NULL;

	if (S_ISBLK(st.st_mode))
		return blkdev__probe(filename, &st);

	fd		= open(filename, readonly ? O_RDONLY : O_RDWR);
	if (fd < 0)
		return NULL;

	disk = qcow_probe(fd, readonly);
	if (disk)
		return disk;

	disk = raw_image__probe(fd, &st, readonly);
	if (disk)
		return disk;

	if (close(fd) < 0)
		warning("close() failed");

	return NULL;
}

void disk_image__close(struct disk_image *disk)
{
	/* If there was no disk image then there's nothing to do: */
	if (!disk)
		return;

	if (disk->ops->close)
		disk->ops->close(disk);

	if (close(disk->fd) < 0)
		warning("close() failed");

	free(disk);
}

/* Fill iov with disk data, starting from sector 'sector'. Return amount of bytes read. */
ssize_t disk_image__read_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	u64 first_sector = sector;

	if (disk->ops->read_sector_iov)
		return disk->ops->read_sector_iov(disk, sector, iov, iovcount);

	while (iovcount--) {
		if (disk->ops->read_sector(disk, sector, iov->iov_base, iov->iov_len) < 0)
			return -1;

		sector += iov->iov_len >> SECTOR_SHIFT;
		iov++;
	}

	return (sector - first_sector) << SECTOR_SHIFT;
}

/* Write iov to disk, starting from sector 'sector'. Return amount of bytes written. */
ssize_t disk_image__write_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	u64 first_sector = sector;

	if (disk->ops->write_sector_iov)
		return disk->ops->write_sector_iov(disk, sector, iov, iovcount);

	while (iovcount--) {
		if (disk->ops->write_sector(disk, sector, iov->iov_base, iov->iov_len) < 0)
			return -1;

		sector += iov->iov_len >> SECTOR_SHIFT;
		iov++;
	}

	return (sector - first_sector) << SECTOR_SHIFT;
}