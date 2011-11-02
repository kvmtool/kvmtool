#include "kvm/disk-image.h"
#include "kvm/qcow.h"
#include "kvm/virtio-blk.h"

#include <sys/eventfd.h>
#include <sys/poll.h>

#define AIO_MAX 32

int debug_iodelay;

#ifdef CONFIG_HAS_AIO
static void *disk_image__thread(void *param)
{
	struct disk_image *disk = param;
	u64 dummy;

	while (read(disk->evt, &dummy, sizeof(dummy)) > 0) {
		struct io_event event;
		struct timespec notime = {0};

		while (io_getevents(disk->ctx, 1, 1, &event, &notime) > 0)
			disk->disk_req_cb(event.data, event.res);
	}

	return NULL;
}
#endif

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
		if (disk->priv == MAP_FAILED) {
			free(disk);
			disk = NULL;
		}
	}

#ifdef CONFIG_HAS_AIO
	if (disk) {
		pthread_t thread;

		disk->evt = eventfd(0, 0);
		io_setup(AIO_MAX, &disk->ctx);
		if (pthread_create(&thread, NULL, disk_image__thread, disk) != 0)
			die("Failed starting IO thread");
	}
#endif
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
	disk		= qcow_probe(fd, true);
	if (disk) {
		pr_warning("Forcing read-only support for QCOW");
		return disk;
	}

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
ssize_t disk_image__read(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	ssize_t total = 0;

	if (debug_iodelay)
		msleep(debug_iodelay);

	if (disk->ops->read_sector) {
		total = disk->ops->read_sector(disk, sector, iov, iovcount, param);
		if (total < 0) {
			pr_info("disk_image__read error: total=%ld\n", (long)total);
			return -1;
		}
	} else {
		/* Do nothing */
	}

	if (!disk->async && disk->disk_req_cb)
		disk->disk_req_cb(param, total);

	return total;
}

/*
 * Write iov to disk, starting from sector 'sector'.
 * Return amount of bytes written.
 */
ssize_t disk_image__write(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	ssize_t total = 0;

	if (debug_iodelay)
		msleep(debug_iodelay);

	if (disk->ops->write_sector) {
		/*
		 * Try writev based operation first
		 */

		total = disk->ops->write_sector(disk, sector, iov, iovcount, param);
		if (total < 0) {
			pr_info("disk_image__write error: total=%ld\n", (long)total);
			return -1;
		}
	} else {
		/* Do nothing */
	}

	if (!disk->async && disk->disk_req_cb)
		disk->disk_req_cb(param, total);

	return total;
}

ssize_t disk_image__get_serial(struct disk_image *disk, void *buffer, ssize_t *len)
{
	struct stat st;

	if (fstat(disk->fd, &st) != 0)
		return 0;

	*len = snprintf(buffer, *len, "%llu%llu%llu", (u64)st.st_dev, (u64)st.st_rdev, (u64)st.st_ino);
	return *len;
}

void disk_image__set_callback(struct disk_image *disk, void (*disk_req_cb)(void *param, long len))
{
	disk->disk_req_cb = disk_req_cb;
}
