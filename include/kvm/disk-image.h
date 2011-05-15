#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include <linux/types.h>
#include <stdbool.h>
#include <sys/uio.h>
#include <unistd.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1UL << SECTOR_SHIFT)

struct disk_image;

struct disk_image_operations {
	int (*read_sector)(struct disk_image *disk, u64 sector, void *dst, u32 dst_len);
	int (*write_sector)(struct disk_image *disk, u64 sector, void *src, u32 src_len);
	ssize_t (*read_sector_iov)(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
	ssize_t (*write_sector_iov)(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
	int (*flush)(struct disk_image *disk);
	void (*close)(struct disk_image *disk);
};

struct disk_image {
	int				fd;
	u64				size;
	struct disk_image_operations	*ops;
	void				*priv;
};

struct disk_image *disk_image__open(const char *filename, bool readonly);
struct disk_image *disk_image__new(int fd, u64 size, struct disk_image_operations *ops);
struct disk_image *disk_image__new_readonly(int fd, u64 size, struct disk_image_operations *ops);
void disk_image__close(struct disk_image *disk);

static inline int disk_image__read_sector(struct disk_image *disk, u64 sector, void *dst, u32 dst_len)
{
	return disk->ops->read_sector(disk, sector, dst, dst_len);
}

static inline int disk_image__write_sector(struct disk_image *disk, u64 sector, void *src, u32 src_len)
{
	return disk->ops->write_sector(disk, sector, src, src_len);
}

ssize_t disk_image__read_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
ssize_t disk_image__write_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);

static inline int disk_image__flush(struct disk_image *disk)
{
	if (disk->ops->flush)
		return disk->ops->flush(disk);
	return fsync(disk->fd);
}

#endif /* KVM__DISK_IMAGE_H */
