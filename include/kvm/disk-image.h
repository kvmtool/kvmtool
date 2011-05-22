#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include "kvm/read-write.h"
#include "kvm/util.h"

#include <linux/types.h>
#include <linux/fs.h>	/* for BLKGETSIZE64 */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <sys/uio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1UL << SECTOR_SHIFT)

#define DISK_IMAGE_MMAP		0
#define DISK_IMAGE_NOMMAP	1
#define MAX_DISK_IMAGES         4

struct disk_image;

struct disk_image_operations {
	/*
	 * The following two are used for reading or writing with a single buffer.
	 * The implentation can use readv/writev or memcpy.
	 */
	ssize_t (*read_sector)(struct disk_image *disk, u64 sector, void *dst, u32 dst_len);
	ssize_t (*write_sector)(struct disk_image *disk, u64 sector, void *src, u32 src_len);
	/*
	 * The following two are used for reading or writing with multiple buffers.
	 * The implentation can use readv/writev or memcpy.
	 */
	ssize_t (*read_sector_iov)(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
	ssize_t (*write_sector_iov)(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
	int (*flush)(struct disk_image *disk);
	int (*close)(struct disk_image *disk);
};

struct disk_image {
	int				fd;
	u64				size;
	struct disk_image_operations	*ops;
	void				*priv;
};

struct disk_image *disk_image__open(const char *filename, bool readonly);
struct disk_image **disk_image__open_all(const char **filenames, bool *readonly, int count);
struct disk_image *disk_image__new(int fd, u64 size, struct disk_image_operations *ops, int mmap);
int disk_image__close(struct disk_image *disk);
void disk_image__close_all(struct disk_image **disks, int count);
int disk_image__flush(struct disk_image *disk);
ssize_t disk_image__read(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
ssize_t disk_image__write(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);

struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly);
struct disk_image *blkdev__probe(const char *filename, struct stat *st);

ssize_t raw_image__read_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
ssize_t raw_image__write_sector_iov(struct disk_image *disk, u64 sector, const struct iovec *iov, int iovcount);
ssize_t raw_image__read_sector(struct disk_image *disk, u64 sector, void *dst, u32 dst_len);
ssize_t raw_image__write_sector(struct disk_image *disk, u64 sector, void *src, u32 src_len);
int raw_image__close(struct disk_image *disk);

#endif /* KVM__DISK_IMAGE_H */
