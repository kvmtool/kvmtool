#include "kvm/disk-image.h"
#include "kvm/qcow.h"

int debug_iodelay;

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
		pr_warning("close() failed");

	return NULL;
}

struct disk_image **disk_image__open_all(const char **filenames, bool *readonly, int count)
{
	struct disk_image **disks;
	int i;

	if (!count || count > MAX_DISK_IMAGES)
		return NULL;

	disks = calloc(count, sizeof(*disks));
	if (!disks)
		return NULL;

	for (i = 0; i < count; i++) {
		if (!filenames[i])
			continue;

		disks[i] = disk_image__open(filenames[i], readonly[i]);
		if (!disks[i]) {
			pr_error("Loading disk image '%s' failed", filenames[i]);
			goto error;
		}
	}
	return disks;
error:
	for (i = 0; i < count; i++)
		disk_image__close(disks[i]);

	free(disks);
	return NULL;
}

int disk_image__flush(struct disk_image *disk)
{
	if (disk->ops->flush)
		return disk->ops->flush(disk);

	return fsync(disk->fd);
}

int disk_image__close(struct disk_image *disk)
{
	/* If there was no disk image then there's nothing to do: */
	if (!disk)
		return 0;

	if (disk->ops->close)
		return disk->ops->close(disk);

	if (close(disk->fd) < 0)
		pr_warning("close() failed");

	free(disk);

	return 0;
}

void disk_image__close_all(struct disk_image **disks, int count)
{
	while (count)
		disk_image__close(disks[--count]);

	free(disks);
}

/*
 * Fill iov with disk data, starting from sector 'sector'.
 * Return amount of bytes read.
 */
ssize_t disk_image__read(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	ssize_t total = 0;
	ssize_t nr;

	if (debug_iodelay)
		msleep(debug_iodelay);

	if (disk->ops->read_sector_iov) {
		/*
		 * Try mulitple buffer based operation first
		 */
		total		= disk->ops->read_sector_iov(disk, sector, iov, iovcount);
		if (total < 0) {
			pr_info("disk_image__read error: total=%ld\n", (long)total);
			return -1;
		}
	} else if (disk->ops->read_sector) {
		/*
		 * Fallback to single buffer based operation
		 */
		while (iovcount--) {
			nr 	= disk->ops->read_sector(disk, sector, iov->iov_base, iov->iov_len);
			if (nr != (ssize_t)iov->iov_len) {
				pr_info("disk_image__read error: nr = %ld iov_len=%ld\n", (long)nr, (long)iov->iov_len);
				return -1;
			}
			sector	+= iov->iov_len >> SECTOR_SHIFT;
			iov++;
			total 	+= nr;
		}
	} else
		die("No disk image operation for read\n");

	return total;
}

/*
 * Write iov to disk, starting from sector 'sector'.
 * Return amount of bytes written.
 */
ssize_t disk_image__write(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount)
{
	ssize_t total = 0;
	ssize_t nr;

	if (debug_iodelay)
		msleep(debug_iodelay);

	if (disk->ops->write_sector_iov) {
		/*
		 * Try writev based operation first
		 */
		total = disk->ops->write_sector_iov(disk, sector, iov, iovcount);
		if (total < 0) {
			pr_info("disk_image__write error: total=%ld\n", (long)total);
			return -1;
		}
	} else if (disk->ops->write_sector) {
		/*
		 * Fallback to single buffer based operation
		 */
		while (iovcount--) {
			nr	 = disk->ops->write_sector(disk, sector, iov->iov_base, iov->iov_len);
			if (nr != (ssize_t)iov->iov_len) {
				pr_info("disk_image__write error: nr=%ld iov_len=%ld\n", (long)nr, (long)iov->iov_len);
				return -1;
			}

			sector	+= iov->iov_len >> SECTOR_SHIFT;
			iov++;
			total	+= nr;
		}
	} else
		die("No disk image operation for read\n");

	return total;
}
