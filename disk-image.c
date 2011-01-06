#include "kvm/disk-image.h"

#include "kvm/util.h"

#include <sys/types.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1UL << SECTOR_SHIFT)

static void setup_geometry(struct disk_image *self)
{
	int cylinders, heads, sectors;
	uint64_t total_sects;

	/*
	 * Set the standart disk geometry of the image.
	 *
	 * Real disk example:
	 *
	 *   Disk /dev/sda: 500.1 GB, 500107862016 bytes
	 *   255 heads, 63 sectors/track, 60801 cylinders, total 976773168 sectors
	 */
	cylinders	= (self->size >> SECTOR_SHIFT) / 16383;
	if (cylinders > 16383)
		cylinders	= 16383;
	else if (cylinders < 2)
		cylinders	= 2;

	heads		= (self->size >> SECTOR_SHIFT) / cylinders;
	if (heads > 255)
		heads		= 255;
	else if (heads < 1)
		heads		= 1;

	sectors		= (self->size >> SECTOR_SHIFT) / cylinders / heads;
	if (sectors > 255)
		sectors		= 255;
	else if (sectors < 1)
		sectors		= 1;

	self->sectors	= sectors;
	self->heads	= heads;
	self->cylinders	= cylinders;

	total_sects	= self->sectors * self->heads * self->cylinders;

	if (total_sects != self->size >> SECTOR_SHIFT)
		warning("Geometry information advertises %" PRIu64 " total sectors but raw image size has %" PRIu64 " sectors",
				total_sects, self->size >> SECTOR_SHIFT);
}

struct disk_image *disk_image__open(const char *filename)
{
	struct disk_image *self;
	struct stat st;

	self		= malloc(sizeof *self);
	if (!self)
		return NULL;

	self->fd	= open(filename, O_RDONLY);
	if (self->fd < 0)
		goto failed_free;

	if (fstat(self->fd, &st) < 0)
		goto failed_close_fd;

	self->size	= st.st_size;

	self->mmap	= mmap(NULL, self->size, PROT_READ, MAP_PRIVATE, self->fd, 0);
	if (self->mmap == MAP_FAILED)
		goto failed_close_fd;

	setup_geometry(self);

	info("block image geometry: sectors: %d heads: %d cylinders: %d",
		self->sectors, self->heads, self->cylinders);

	return self;

failed_close_fd:
	close(self->fd);
failed_free:
	free(self);

	return NULL;
}

void disk_image__close(struct disk_image *self)
{
	if (munmap(self->mmap, self->size) < 0)
		warning("munmap() failed");

	if (close(self->fd) < 0)
		warning("close() failed");

	free(self);
}

int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len)
{
	uint64_t offset = sector << SECTOR_SHIFT;

	if (offset + dst_len > self->size)
		return -1;

	memcpy(dst, self->mmap + offset, dst_len);

	return 0;
}
