#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include <stdint.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1UL << SECTOR_SHIFT)

struct disk_image {
	void		*mmap;
	int		fd;
	uint64_t	size;
};

struct disk_image *disk_image__open(const char *filename);
void disk_image__close(struct disk_image *self);
int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len);
int disk_image__write_sector(struct disk_image *self, uint64_t sector, void *src, uint32_t src_len);

#endif /* KVM__DISK_IMAGE_H */
