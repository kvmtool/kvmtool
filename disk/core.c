#include "kvm/disk-image.h"
#include "kvm/qcow.h"

struct disk_image *disk_image__new(int fd, u64 size, struct disk_image_operations *ops, int use_mmap)
{
	struct disk_image *disk;

	disk		= malloc(sizeof *disk);
	if (!disk)
		return NULL;

	disk->fd	= fd;
	disk->size	= size;
	disk->ops	= ops;

	if (use_mmap == DISK_IMAGE_MMAP) {
		/*
		 * The write to disk image will be discarded
		 */
		disk->priv = mmap(NULL, size, PROT_RW, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
		if (disk->priv == MAP_FAILED)
			die("mmap() failed");
	}

	return disk;
}

struct disk_image *disk_image__open(const char *filename, bool readonly)
{
	struct disk_image *disk;
	struct stat st;
	int fd;

	if (stat(filename, &st) < 0)
		return NULL;

	/* blk device ?*/
	disk		= blkdev__probe(filename, &st);
	if (disk)
		return disk;

	fd		= open(filename, readonly ? O_RDONLY : O_RDWR);
	if (fd < 0)
		return NULL;

	/* qcow image ?*/
	disk		= qcow_probe(fd, readonly);
	if (disk)
		return disk;

	/* raw image ?*/
	disk		= raw_image__probe(fd, &st, readonly);
	if (disk)
		return disk;

	if (close(fd) < 0)
		warning("close() failed");

	return NULL;
}

int disk_image__close(struct disk_image *disk)
{
	/* If there was no disk image then there's nothing to do: */
	if (!disk)
		return 0;

	if (disk->ops->close)
		return disk->ops->close(disk);

	if (close(disk->fd) < 0)
		warning("close() failed");

	free(disk);

	return 0;
}

/* Fill iov with disk data, starting from sector 'sector'. Return amount of bytes read. */
ssize_t disk_image__read(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
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
ssize_t disk_image__write(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
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
