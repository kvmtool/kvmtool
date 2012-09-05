#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include "kvm/read-write.h"
#include "kvm/util.h"
#include "kvm/parse-options.h"

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

enum {
	DISK_IMAGE_REGULAR,
	DISK_IMAGE_MMAP,
};

#define MAX_DISK_IMAGES         4

struct disk_image;

struct disk_image_operations {
	ssize_t (*read)(struct disk_image *disk, u64 sector, const struct iovec *iov,
			int iovcount, void *param);
	ssize_t (*write)(struct disk_image *disk, u64 sector, const struct iovec *iov,
			int iovcount, void *param);
	int (*flush)(struct disk_image *disk);
	int (*close)(struct disk_image *disk);
};

struct disk_image_params {
	const char *filename;
	/*
	 * wwpn == World Wide Port Number
	 * tpgt == Target Portal Group Tag
	 */
	const char *wwpn;
	const char *tpgt;
	bool readonly;
	bool direct;
};

struct disk_image {
	int				fd;
	u64				size;
	struct disk_image_operations	*ops;
	void				*priv;
	void				*disk_req_cb_param;
	void				(*disk_req_cb)(void *param, long len);
	bool				async;
	int				evt;
#ifdef CONFIG_HAS_AIO
	io_context_t			ctx;
#endif
	const char			*wwpn;
	const char			*tpgt;
	int				debug_iodelay;
};

int disk_img_name_parser(const struct option *opt, const char *arg, int unset);
int disk_image__init(struct kvm *kvm);
int disk_image__exit(struct kvm *kvm);
struct disk_image *disk_image__new(int fd, u64 size, struct disk_image_operations *ops, int mmap);
int disk_image__flush(struct disk_image *disk);
ssize_t disk_image__read(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param);
ssize_t disk_image__write(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param);
ssize_t disk_image__get_serial(struct disk_image *disk, void *buffer, ssize_t *len);

struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly);
struct disk_image *blkdev__probe(const char *filename, int flags, struct stat *st);

ssize_t raw_image__read(struct disk_image *disk, u64 sector,
				const struct iovec *iov, int iovcount, void *param);
ssize_t raw_image__write(struct disk_image *disk, u64 sector,
				const struct iovec *iov, int iovcount, void *param);
ssize_t raw_image__read_mmap(struct disk_image *disk, u64 sector,
				const struct iovec *iov, int iovcount, void *param);
ssize_t raw_image__write_mmap(struct disk_image *disk, u64 sector,
				const struct iovec *iov, int iovcount, void *param);
int raw_image__close(struct disk_image *disk);
void disk_image__set_callback(struct disk_image *disk, void (*disk_req_cb)(void *param, long len));
#endif /* KVM__DISK_IMAGE_H */
