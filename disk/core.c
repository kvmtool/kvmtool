#include "kvm/disk-image.h"
#include "kvm/qcow.h"

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
