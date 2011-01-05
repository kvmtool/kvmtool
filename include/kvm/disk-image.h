#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include <stdint.h>

struct disk_image {
	void		*mmap;
	int		fd;
	uint64_t	size;
};

struct disk_image *disk_image__open(const char *filename);
void disk_image__close(struct disk_image *self);
int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst);

#endif /* KVM__DISK_IMAGE_H */
