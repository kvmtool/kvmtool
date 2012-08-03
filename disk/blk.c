#include "kvm/disk-image.h"

#include <linux/err.h>
#include <mntent.h>

/*
 * raw image and blk dev are similar, so reuse raw image ops.
 */
static struct disk_image_operations blk_dev_ops = {
	.read	= raw_image__read,
	.write	= raw_image__write,
};

static bool is_mounted(struct stat *st)
{
	struct stat st_buf;
	struct mntent *mnt;
	FILE *f;

	f = setmntent("/proc/mounts", "r");
	if (!f)
		return false;

	while ((mnt = getmntent(f)) != NULL) {
		if (stat(mnt->mnt_fsname, &st_buf) == 0 &&
		    S_ISBLK(st_buf.st_mode) && st->st_rdev == st_buf.st_rdev) {
			fclose(f);
			return true;
		}
	}

	fclose(f);
	return false;
}

struct disk_image *blkdev__probe(const char *filename, int flags, struct stat *st)
{
	struct disk_image *disk;
	int fd, r;
	u64 size;

	if (!S_ISBLK(st->st_mode))
		return ERR_PTR(-EINVAL);

	if (is_mounted(st)) {
		pr_err("Block device %s is already mounted! Unmount before use.",
		       filename);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * Be careful! We are opening host block device!
	 * Open it readonly since we do not want to break user's data on disk.
	 */
	fd = open(filename, flags);
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
	disk = disk_image__new(fd, size, &blk_dev_ops, DISK_IMAGE_REGULAR);
#ifdef CONFIG_HAS_AIO
		if (!IS_ERR_OR_NULL(disk))
			disk->async = 1;
#endif
	return disk;
}
