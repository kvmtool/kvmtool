#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include <stdint.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1UL << SECTOR_SHIFT)

struct disk_image;

struct disk_image_operations {
	int (*read_sector)(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len);
	int (*write_sector)(struct disk_image *self, uint64_t sector, void *src, uint32_t src_len);
	void (*close)(struct disk_image *self);
};

struct disk_image {
	int				fd;
	uint64_t			size;
	struct disk_image_operations	*ops;
	void				*priv;
};

struct disk_image *disk_image__open(const char *filename);
struct disk_image *disk_image__new(int fd, uint64_t size);
void disk_image__close(struct disk_image *self);

static inline int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len)
{
	return self->ops->read_sector(self, sector, dst, dst_len);
}

static inline int disk_image__write_sector(struct disk_image *self, uint64_t sector, void *src, uint32_t src_len)
{
	return self->ops->write_sector(self, sector, src, src_len);
}

#endif /* KVM__DISK_IMAGE_H */
