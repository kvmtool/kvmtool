#ifndef KVM__DISK_IMAGE_H
#define KVM__DISK_IMAGE_H

#include <stdint.h>

struct disk_image {
	void		*mmap;
	int		fd;
	uint64_t	size;

	uint16_t	cylinders;
	uint8_t		heads;
	uint8_t		sectors;
};

struct disk_image *disk_image__open(const char *filename);
void disk_image__close(struct disk_image *self);
int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len);

#endif /* KVM__DISK_IMAGE_H */
